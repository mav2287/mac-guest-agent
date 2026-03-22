#include "commands.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMMANDS 64

static command_entry_t registry[MAX_COMMANDS];
static int num_commands = 0;

/* Forward declaration */
static command_entry_t *find_command(const char *name);

/* Forward declarations for all cmd-*.c init functions */
void cmd_info_init(void);
void cmd_system_init(void);
void cmd_power_init(void);
void cmd_hardware_init(void);
void cmd_disk_init(void);
void cmd_fs_init(void);
void cmd_network_init(void);
void cmd_file_init(void);
void cmd_exec_init(void);
void cmd_ssh_init(void);
void cmd_user_init(void);

void command_register(const char *name, command_handler_fn handler, int enabled)
{
    if (num_commands >= MAX_COMMANDS) {
        LOG_ERROR("Command registry full, cannot register: %s", name);
        return;
    }
    registry[num_commands].name = name;
    registry[num_commands].handler = handler;
    registry[num_commands].enabled = enabled;
    num_commands++;
    LOG_DEBUG("Registered command: %s", name);
}

void commands_init(void)
{
    cmd_info_init();
    cmd_system_init();
    cmd_power_init();
    cmd_hardware_init();
    cmd_disk_init();
    cmd_fs_init();
    cmd_network_init();
    cmd_file_init();
    cmd_exec_init();
    cmd_ssh_init();
    cmd_user_init();
    LOG_INFO("Registered %d commands", num_commands);
}

void commands_apply_filters(const char *block_rpcs, const char *allow_rpcs)
{
    if (!block_rpcs && !allow_rpcs)
        return;

    if (allow_rpcs && *allow_rpcs) {
        /* Allow-list mode: disable everything, then enable listed commands */
        for (int i = 0; i < num_commands; i++)
            registry[i].enabled = 0;

        /* Parse comma-separated allow list */
        char *list = safe_strdup(allow_rpcs);
        char *save = NULL;
        char *tok = strtok_r(list, ",", &save);
        int allowed = 0;
        while (tok) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';

            command_entry_t *cmd = find_command(tok);
            if (cmd) {
                cmd->enabled = 1;
                allowed++;
            } else {
                LOG_WARN("allow-rpcs: unknown command '%s'", tok);
            }
            tok = strtok_r(NULL, ",", &save);
        }
        free(list);
        LOG_INFO("allow-rpcs: %d commands enabled, %d disabled", allowed, num_commands - allowed);
    } else if (block_rpcs && *block_rpcs) {
        /* Block-list mode: disable listed commands */
        char *list = safe_strdup(block_rpcs);
        char *save = NULL;
        char *tok = strtok_r(list, ",", &save);
        int blocked = 0;
        while (tok) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';

            command_entry_t *cmd = find_command(tok);
            if (cmd) {
                cmd->enabled = 0;
                blocked++;
            } else {
                LOG_WARN("block-rpcs: unknown command '%s'", tok);
            }
            tok = strtok_r(NULL, ",", &save);
        }
        free(list);
        LOG_INFO("block-rpcs: %d commands disabled", blocked);
    }
}

static command_entry_t *find_command(const char *name)
{
    for (int i = 0; i < num_commands; i++) {
        if (strcmp(registry[i].name, name) == 0)
            return &registry[i];
    }
    return NULL;
}

char *commands_dispatch(const char *cmd_name, cJSON *args, const cJSON *id)
{
    if (!cmd_name) {
        return protocol_build_error("CommandNotFound", "No command specified", id);
    }

    command_entry_t *cmd = find_command(cmd_name);
    if (!cmd || !cmd->enabled) {
        LOG_ERROR("Command not found or disabled: %s", cmd_name);
        char desc[256];
        snprintf(desc, sizeof(desc), "The command %s has not been found", cmd_name);
        return protocol_build_error("CommandNotFound", desc, id);
    }

    LOG_DEBUG("Handling command: %s", cmd_name);

    const char *err_class = NULL;
    const char *err_desc = NULL;
    cJSON *result = cmd->handler(args, &err_class, &err_desc);

    if (err_class || err_desc) {
        if (result) cJSON_Delete(result);
        return protocol_build_error(
            err_class ? err_class : "GenericError",
            err_desc ? err_desc : "Command execution failed",
            id
        );
    }

    return protocol_build_response(result, id);
}

cJSON *commands_get_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num_commands; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", registry[i].name);
        cJSON_AddBoolToObject(item, "enabled", registry[i].enabled);
        cJSON_AddBoolToObject(item, "success-response", 1);
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}

int commands_count(void)
{
    return num_commands;
}
