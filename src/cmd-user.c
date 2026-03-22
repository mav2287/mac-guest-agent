#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *handle_set_user_password(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(args, "username");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(args, "password");
    cJSON *crypted_item = cJSON_GetObjectItemCaseSensitive(args, "crypted");

    if (!cJSON_IsString(user_item) || !user_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'username' argument";
        return NULL;
    }
    if (!cJSON_IsString(pass_item) || !pass_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'password' argument";
        return NULL;
    }

    const char *username = user_item->valuestring;
    const char *password = pass_item->valuestring;

    /* If password is base64 encoded (crypted=false means plain base64 in QGA protocol) */
    char *decoded_pass = NULL;
    if (!cJSON_IsTrue(crypted_item)) {
        size_t decoded_len;
        unsigned char *raw = base64_decode(password, &decoded_len);
        if (raw) {
            decoded_pass = (char *)raw;
            password = decoded_pass;
        }
    }

    /* Validate username - prevent injection */
    for (const char *p = username; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-' && *p != '.') {
            free(decoded_pass);
            *err_class = "InvalidParameter";
            *err_desc = "Invalid username characters";
            return NULL;
        }
    }

    /* Use dscl to change password (available on all macOS) */
    char *const argv[] = {
        "dscl", ".", "-passwd",
        (char *)(void *)username,  /* user path is just the username for dscl */
        (char *)(void *)password,
        NULL
    };

    /* Build the proper user path */
    char user_path[256];
    snprintf(user_path, sizeof(user_path), "/Users/%s", username);

    char *const real_argv[] = {
        "dscl", ".", "-passwd", user_path, (char *)(void *)password, NULL
    };

    char *cmd_out = NULL;
    int rc = run_command_v("dscl", real_argv, &cmd_out, NULL);
    free(cmd_out);
    free(decoded_pass);

    (void)argv; /* suppress unused warning */

    if (rc != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to set user password via dscl";
        return NULL;
    }

    LOG_DEBUG("Password changed for user %s", username);
    return cJSON_CreateObject();
}

void cmd_user_init(void)
{
    command_register("guest-set-user-password", handle_set_user_password, 1);
}
