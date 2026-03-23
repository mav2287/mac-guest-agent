#include "selftest.h"
#include "agent.h"
#include "compat.h"
#include "commands.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define HOOK_DIR "/etc/qemu/fsfreeze-hook.d"
#define CONFIG_PATH "/etc/qemu/qemu-ga.conf"

static int warnings = 0;
static int errors = 0;

static void check_pass(const char *name, const char *detail)
{
    printf("  PASS  %s", name);
    if (detail && detail[0])
        printf(": %s", detail);
    printf("\n");
}

static void check_warn(const char *name, const char *detail)
{
    printf("  WARN  %s", name);
    if (detail && detail[0])
        printf(": %s", detail);
    printf("\n");
    warnings++;
}

static void check_fail(const char *name, const char *detail)
{
    printf("  FAIL  %s", name);
    if (detail && detail[0])
        printf(": %s", detail);
    printf("\n");
    errors++;
}

static void check_info(const char *name, const char *detail)
{
    printf("  INFO  %s", name);
    if (detail && detail[0])
        printf(": %s", detail);
    printf("\n");
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_writable(const char *path)
{
    return access(path, W_OK) == 0;
}

static int tool_available(const char *name)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

static void check_version(void)
{
    printf("\n[Version]\n");
    printf("  agent: mac-guest-agent %s\n", AGENT_VERSION);

    compat_init();
    const os_version_t *ver = compat_os_version();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d", ver->major, ver->minor, ver->patch);

    if (ver->major == 0) {
        check_fail("macOS version", "could not detect (sw_vers failed)");
    } else {
        check_pass("macOS version", buf);
    }

    struct utsname uts;
    if (uname(&uts) == 0) {
        char arch_buf[128];
        snprintf(arch_buf, sizeof(arch_buf), "%s (%s)", uts.machine, uts.sysname);
        check_pass("architecture", arch_buf);
    } else {
        check_warn("architecture", "uname failed");
    }
}

static void check_privileges(void)
{
    printf("\n[Privileges]\n");
    if (geteuid() == 0) {
        check_pass("running as root", "yes");
    } else {
        check_warn("running as root", "no (some commands require root)");
    }
}

static void check_serial_device(void)
{
    printf("\n[Serial Device]\n");

    const char *isa_devices[] = {
        "/dev/cu.serial1",
        "/dev/cu.serial2",
        "/dev/cu.serial",
        NULL
    };

    const char *virtio_devices[] = {
        "/dev/cu.org.qemu.guest_agent.0",
        "/dev/tty.org.qemu.guest_agent.0",
        NULL
    };

    int found_isa = 0;
    int found_virtio = 0;

    for (int i = 0; isa_devices[i]; i++) {
        if (file_exists(isa_devices[i])) {
            char detail[256];
            int readable = access(isa_devices[i], R_OK) == 0;
            int writable = access(isa_devices[i], W_OK) == 0;
            snprintf(detail, sizeof(detail), "%s (r=%s w=%s)",
                     isa_devices[i],
                     readable ? "yes" : "no",
                     writable ? "yes" : "no");
            check_pass("ISA serial device", detail);
            if (!readable || !writable)
                check_warn("ISA serial permissions", "device not fully accessible");
            found_isa = 1;
            break;
        }
    }

    for (int i = 0; virtio_devices[i]; i++) {
        if (file_exists(virtio_devices[i])) {
            check_info("VirtIO serial device", virtio_devices[i]);
            found_virtio = 1;
            break;
        }
    }

    if (!found_isa && !found_virtio) {
        check_warn("serial device", "no ISA or VirtIO serial device found (expected in VM with agent enabled)");
    } else if (!found_isa && found_virtio) {
        check_info("transport", "VirtIO only (type=isa recommended for broader compatibility)");
    }
}

static void check_config(void)
{
    printf("\n[Configuration]\n");
    if (file_exists(CONFIG_PATH)) {
        check_pass("config file", CONFIG_PATH);
    } else {
        check_info("config file", "not present (using defaults)");
    }
}

static void check_log(void)
{
    printf("\n[Logging]\n");
    const char *log_path = "/var/log/mac-guest-agent.log";

    if (file_exists(log_path)) {
        if (file_writable(log_path)) {
            check_pass("log file", log_path);
        } else {
            check_warn("log file", "exists but not writable");
        }
    } else {
        /* Check if the directory is writable */
        if (file_writable("/var/log")) {
            check_pass("log directory", "/var/log is writable (log file will be created)");
        } else {
            check_warn("log directory", "/var/log not writable (need root)");
        }
    }
}

static void check_filesystem(void)
{
    printf("\n[Filesystem]\n");

    if (compat_has_apfs()) {
        check_pass("APFS support", "yes (10.13+)");

        if (compat_has_tmutil()) {
            check_pass("tmutil snapshots", "available (freeze will create APFS snapshot)");
        } else {
            check_warn("tmutil snapshots", "tmutil not executable (freeze will use sync-only)");
        }
    } else {
        check_info("APFS support", "no (pre-10.13, freeze will use sync + F_FULLFSYNC only)");
    }
}

static void check_hooks(void)
{
    printf("\n[Freeze Hooks]\n");
    struct stat st;

    if (stat(HOOK_DIR, &st) != 0) {
        check_info("hook directory", "not present (no hooks configured)");
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        check_fail("hook directory", HOOK_DIR " exists but is not a directory");
        return;
    }

    check_pass("hook directory", HOOK_DIR);

    /* Count and validate hook scripts */
    char *output = NULL;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>/dev/null", HOOK_DIR);
    if (run_command_capture(cmd, &output) == 0 && output && output[0]) {
        int count = 0;
        int valid = 0;
        int invalid = 0;
        char *save = NULL;
        char *line = strtok_r(output, "\n", &save);
        while (line) {
            count++;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", HOOK_DIR, line);
            struct stat fst;
            if (stat(path, &fst) == 0) {
                int ok = 1;
                /* Must be owned by root */
                if (fst.st_uid != 0) ok = 0;
                /* Must not be world-writable */
                if (fst.st_mode & S_IWOTH) ok = 0;
                /* Must be executable */
                if (!(fst.st_mode & S_IXUSR)) ok = 0;

                if (ok) {
                    valid++;
                } else {
                    char detail[512];
                    snprintf(detail, sizeof(detail), "%s (uid=%d, mode=%04o)", line,
                             fst.st_uid, fst.st_mode & 0777);
                    check_warn("hook script", detail);
                    invalid++;
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }
        char summary[128];
        snprintf(summary, sizeof(summary), "%d script(s), %d valid, %d invalid", count, valid, invalid);
        if (invalid > 0)
            check_warn("hook scripts", summary);
        else
            check_pass("hook scripts", summary);
        free(output);
    } else {
        check_info("hook scripts", "none");
        free(output);
    }
}

static void check_commands(void)
{
    printf("\n[Commands]\n");

    commands_init();
    int count = commands_count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d registered", count);
    check_pass("commands", buf);
}

static void check_tools(void)
{
    printf("\n[Required Tools]\n");

    struct { const char *name; const char *purpose; int required; } tools[] = {
        {"sw_vers",    "OS version detection",    1},
        {"sysctl",     "hardware info",           1},
        {"diskutil",   "disk information",        1},
        {"netstat",    "network info (fallback)", 0},
        {"osascript",  "graceful shutdown",       0},
        {"shutdown",   "shutdown fallback",       1},
        {"pmset",      "suspend/hibernate",       0},
        {"dscl",       "password changes",        0},
        {"launchctl",  "service management",      1},
        {"tmutil",     "APFS snapshots",          0},
        {NULL, NULL, 0}
    };

    for (int i = 0; tools[i].name; i++) {
        char detail[256];
        if (tool_available(tools[i].name)) {
            snprintf(detail, sizeof(detail), "%s (%s)", tools[i].name, tools[i].purpose);
            check_pass("tool", detail);
        } else {
            snprintf(detail, sizeof(detail), "%s (%s)", tools[i].name, tools[i].purpose);
            if (tools[i].required) {
                check_fail("tool missing", detail);
            } else {
                check_warn("tool missing", detail);
            }
        }
    }
}

static void check_service(void)
{
    printf("\n[Service]\n");
    const char *plist = "/Library/LaunchDaemons/com.macos.guest-agent.plist";

    if (file_exists(plist)) {
        check_pass("LaunchDaemon plist", "installed");

        /* Check if service is loaded */
        char *output = NULL;
        if (run_command_capture("launchctl list com.macos.guest-agent 2>/dev/null", &output) == 0 && output) {
            if (strstr(output, "com.macos.guest-agent")) {
                check_pass("service status", "loaded");
            } else {
                check_info("service status", "not loaded");
            }
            free(output);
        } else {
            check_info("service status", "not loaded (or not running as root)");
            free(output);
        }
    } else {
        check_info("LaunchDaemon plist", "not installed (run --install)");
    }
}

static void check_environment(void)
{
    printf("\n[Environment]\n");

    /* Check hw.model */
    char *model = NULL;
    if (run_command_capture("sysctl -n hw.model", &model) == 0 && model) {
        char *nl = strchr(model, '\n');
        if (nl) *nl = '\0';
        check_info("hardware model", model);
        free(model);
    } else {
        check_warn("hardware model", "could not detect");
        free(model);
    }

    /* QEMU indicator check */
    char *profiler = NULL;
    int qemu_detected = 0;

    if (run_command_capture("sysctl -n hw.model", &profiler) == 0 && profiler) {
        for (char *p = profiler; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (strstr(profiler, "QEMU"))
            qemu_detected = 1;
        free(profiler);
    }

    if (!qemu_detected) {
        const char *devices[] = {
            "/dev/cu.org.qemu.guest_agent.0",
            "/dev/tty.org.qemu.guest_agent.0",
            "/dev/virtio", "/dev/vda", "/dev/vdb", NULL
        };
        for (int i = 0; devices[i]; i++) {
            if (file_exists(devices[i])) {
                qemu_detected = 1;
                break;
            }
        }
    }

    if (qemu_detected) {
        check_pass("QEMU environment", "detected");
    } else {
        check_info("QEMU environment", "not detected (normal for macOS VMs with custom hardware models)");
    }
}

int selftest_run(void)
{
    printf("mac-guest-agent %s self-test\n", AGENT_VERSION);
    printf("================================\n");

    check_version();
    check_privileges();
    check_environment();
    check_serial_device();
    check_config();
    check_log();
    check_filesystem();
    check_hooks();
    check_commands();
    check_tools();
    check_service();

    printf("\n================================\n");
    printf("Result: %d error(s), %d warning(s)\n", errors, warnings);

    if (errors > 0) {
        printf("Status: PROBLEMS DETECTED\n");
        return 1;
    } else if (warnings > 0) {
        printf("Status: OK (with warnings)\n");
        return 0;
    } else {
        printf("Status: ALL CHECKS PASSED\n");
        return 0;
    }
}
