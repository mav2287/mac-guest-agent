#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static cJSON *handle_shutdown(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)err_class; (void)err_desc;

    const char *mode = "powerdown";
    if (args) {
        cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(args, "mode");
        if (cJSON_IsString(mode_item) && mode_item->valuestring)
            mode = mode_item->valuestring;
    }

    LOG_INFO("Shutdown requested, mode=%s", mode);

    /* Fork to execute shutdown after response is sent */
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork() failed for shutdown: %s", strerror(errno));
        *err_class = "GenericError";
        *err_desc = "Failed to fork for shutdown";
        return NULL;
    } else if (pid == 0) {
        /* Child: brief delay to let response go out, then execute */
        usleep(200000);
        setsid();

        if (strcmp(mode, "reboot") == 0) {
            /* Try graceful AppleScript reboot first */
            char *const av1[] = { "osascript", "-e",
                "tell app \"System Events\" to restart", NULL };
            if (run_command_v("osascript", av1, NULL, NULL) != 0) {
                char *const av2[] = { "shutdown", "-r", "now", NULL };
                run_command_v("shutdown", av2, NULL, NULL);
            }
        } else {
            /* powerdown / halt */
            char *const av1[] = { "osascript", "-e",
                "tell app \"System Events\" to shut down", NULL };
            if (run_command_v("osascript", av1, NULL, NULL) != 0) {
                char *const av2[] = { "shutdown", "-h", "now", NULL };
                run_command_v("shutdown", av2, NULL, NULL);
            }
        }
        _exit(0);
    }

    return cJSON_CreateObject();
}

static cJSON *do_suspend(const char *hibernate_mode, const char **err_class, const char **err_desc)
{
    /* Save current hibernatemode so we can restore it after wake */
    char *saved_mode = NULL;
    run_command_capture("pmset -g | grep hibernatemode | awk '{print $2}'", &saved_mode);
    if (saved_mode) {
        char *nl = strchr(saved_mode, '\n');
        if (nl) *nl = '\0';
    }

    char *const set_argv[] = {"pmset", "-a", "hibernatemode", (char *)hibernate_mode, NULL};
    if (run_command_v("/usr/bin/pmset", set_argv, NULL, NULL) != 0) {
        free(saved_mode);
        *err_class = "GenericError";
        *err_desc = "Failed to set hibernate mode";
        return NULL;
    }
    if (run_command("pmset sleepnow") != 0) {
        /* Restore before returning error */
        if (saved_mode && saved_mode[0]) {
            char *const restore_argv[] = {"pmset", "-a", "hibernatemode", saved_mode, NULL};
            run_command_v("/usr/bin/pmset", restore_argv, NULL, NULL);
        }
        free(saved_mode);
        *err_class = "GenericError";
        *err_desc = "Failed to initiate sleep";
        return NULL;
    }

    /* Restore original hibernatemode after sleep returns (VM wakes) */
    if (saved_mode && saved_mode[0]) {
        char *const restore_argv[] = {"pmset", "-a", "hibernatemode", saved_mode, NULL};
        run_command_v("/usr/bin/pmset", restore_argv, NULL, NULL);
    }
    free(saved_mode);
    return cJSON_CreateObject();
}

static cJSON *handle_suspend_disk(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Suspend to disk requested");
    return do_suspend("25", err_class, err_desc);
}

/* guest-suspend-ram / -hybrid are GATED on macOS guests.
 *
 * suspend-disk (hibernatemode 25) writes RAM to the hibernation image and
 * powers the VM off cleanly — the host can cold-start it again, so it behaves
 * like a resumable shutdown and is safe.
 *
 * suspend-ram (hibernatemode 0) and suspend-hybrid (hibernatemode 3) instead
 * tell Darwin to enter S3-style sleep with RAM kept powered, then wait for a
 * hardware wake event. Under QEMU there is no PM wake path back into the guest:
 * `pmset sleepnow` parks the vCPU and the VM wedges with no way to resume from
 * inside the guest. The correct way to suspend a VM to RAM is host-side
 * (`qm suspend <vmid>` on PVE, or `virsh suspend <dom>` on libvirt), which
 * freezes the whole VM at the hypervisor and is resumed with `qm resume` /
 * `virsh resume`. Refuse cleanly here rather than hang the guest. */
static cJSON *handle_suspend_ram(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Suspend to RAM requested — refused (gated on macOS guest)");
    *err_class = "GenericError";
    *err_desc = "guest-suspend-ram is not supported on a macOS guest: in-guest "
                "S3 sleep has no QEMU wake path and wedges the VM. Suspend to "
                "RAM from the host instead (PVE: 'qm suspend <vmid>' / libvirt: "
                "'virsh suspend <domain>'; resume with 'qm resume' / 'virsh "
                "resume'). For a resumable in-guest suspend use guest-suspend-disk.";
    return NULL;
}

static cJSON *handle_suspend_hybrid(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Hybrid suspend requested — refused (gated on macOS guest)");
    *err_class = "GenericError";
    *err_desc = "guest-suspend-hybrid is not supported on a macOS guest: it "
                "relies on in-guest S3 sleep, which has no QEMU wake path and "
                "wedges the VM. Suspend from the host instead (PVE: 'qm suspend "
                "<vmid>' / libvirt: 'virsh suspend <domain>'). For a resumable "
                "in-guest suspend use guest-suspend-disk.";
    return NULL;
}

void cmd_power_init(void)
{
    command_register("guest-shutdown", handle_shutdown, 1);
    /* All three in-guest suspend variants are registered but enabled=0 by
     * default. QEMU/OpenCore macOS guests have no working wake-from-S3/S4 path
     * (PCIe/USB don't re-enumerate on resume; OpenCore/OVMF doesn't restore the
     * hibernate image), so a real suspend wedges the VM (verified on Leopard in
     * docs/evidence/v2.5.5/os-level-destructive.md — required qm rollback) and a
     * config that instant-wakes returns a fake success. The reliable suspend
     * for a managed VM is HOST-SIDE: `qm suspend` / `virsh suspend`. enabled=0
     * = blocked by default; an operator whose guest has a proven wake path can
     * enable it via the allow-rpcs config. Unlike CPU/memory hotplug (never
     * possible on macOS → unregistered), suspend is a genuine but
     * config-dependent capability, so we keep it registered-disabled. */
    command_register("guest-suspend-disk", handle_suspend_disk, 0);
    command_register("guest-suspend-ram", handle_suspend_ram, 0);
    command_register("guest-suspend-hybrid", handle_suspend_hybrid, 0);
}
