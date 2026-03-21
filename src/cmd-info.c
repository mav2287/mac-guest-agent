#include "commands.h"
#include "agent.h"
#include <stdlib.h>
#include <string.h>

static cJSON *handle_ping(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    return cJSON_CreateObject();
}

static cJSON *handle_sync(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)err_class; (void)err_desc;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(args, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing or invalid 'id' argument";
        return NULL;
    }
    /* Return the id value directly as the response */
    return cJSON_CreateNumber(id_item->valuedouble);
}

static cJSON *handle_info(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "version", AGENT_VERSION);
    cJSON *cmd_list = commands_get_list();
    cJSON_AddItemToObject(info, "supported_commands", cmd_list);
    return info;
}

void cmd_info_init(void)
{
    command_register("guest-ping", handle_ping, 1);
    command_register("guest-sync", handle_sync, 1);
    command_register("guest-sync-id", handle_sync, 1);
    command_register("guest-sync-delimited", handle_sync, 1);
    command_register("guest-info", handle_info, 1);
}
