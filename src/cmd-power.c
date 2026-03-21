#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (pid == 0) {
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
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pmset -a hibernatemode %s", hibernate_mode);
    if (run_command(cmd) != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to set hibernate mode";
        return NULL;
    }
    if (run_command("pmset sleepnow") != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to initiate sleep";
        return NULL;
    }
    return cJSON_CreateObject();
}

static cJSON *handle_suspend_disk(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Suspend to disk requested");
    return do_suspend("25", err_class, err_desc);
}

static cJSON *handle_suspend_ram(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Suspend to RAM requested");
    return do_suspend("0", err_class, err_desc);
}

static cJSON *handle_suspend_hybrid(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    LOG_INFO("Hybrid suspend requested");
    return do_suspend("3", err_class, err_desc);
}

void cmd_power_init(void)
{
    command_register("guest-shutdown", handle_shutdown, 1);
    command_register("guest-suspend-disk", handle_suspend_disk, 1);
    command_register("guest-suspend-ram", handle_suspend_ram, 1);
    command_register("guest-suspend-hybrid", handle_suspend_hybrid, 1);
}
