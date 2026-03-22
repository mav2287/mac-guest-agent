#include "protocol.h"
#include <stdlib.h>
#include <string.h>

cJSON *protocol_parse_request(const char *data)
{
    if (!data || !*data)
        return NULL;

    /* Skip leading 0xFF delimiter bytes (used by guest-sync-delimited) */
    while (*data == '\xff')
        data++;

    return cJSON_Parse(data);
}

const char *protocol_get_command(const cJSON *request)
{
    if (!request) return NULL;
    cJSON *exec = cJSON_GetObjectItemCaseSensitive(request, "execute");
    if (cJSON_IsString(exec) && exec->valuestring)
        return exec->valuestring;
    return NULL;
}

cJSON *protocol_get_arguments(const cJSON *request)
{
    if (!request) return NULL;
    return cJSON_GetObjectItemCaseSensitive(request, "arguments");
}

cJSON *protocol_get_id(const cJSON *request)
{
    if (!request) return NULL;
    return cJSON_GetObjectItemCaseSensitive(request, "id");
}

char *protocol_build_response(cJSON *return_value, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        if (return_value) cJSON_Delete(return_value);
        return NULL;
    }

    cJSON *ret = return_value ? return_value : cJSON_CreateObject();
    if (!ret) {
        cJSON_Delete(resp);
        return NULL;
    }
    cJSON_AddItemToObject(resp, "return", ret);

    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    }

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

char *protocol_build_error(const char *error_class, const char *desc, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return NULL;

    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "class", error_class ? error_class : "GenericError");
    cJSON_AddStringToObject(err, "desc", desc ? desc : "Unknown error");
    cJSON_AddItemToObject(resp, "error", err);

    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    }

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return str;
}

char *protocol_build_empty_response(const cJSON *id)
{
    return protocol_build_response(NULL, id);
}
