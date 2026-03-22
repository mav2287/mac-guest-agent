#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <errno.h>

static char *get_authorized_keys_path(const char *username)
{
    struct passwd *pw = getpwnam(username);
    if (!pw) return NULL;

    size_t len = strlen(pw->pw_dir) + strlen("/.ssh/authorized_keys") + 1;
    char *path = malloc(len);
    if (path)
        snprintf(path, len, "%s/.ssh/authorized_keys", pw->pw_dir);
    return path;
}

static cJSON *handle_ssh_get_keys(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(args, "username");
    if (!cJSON_IsString(user_item) || !user_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'username' argument";
        return NULL;
    }

    char *path = get_authorized_keys_path(user_item->valuestring);
    if (!path) {
        *err_class = "GenericError";
        *err_desc = "User not found";
        return NULL;
    }

    cJSON *keys = cJSON_CreateArray();
    char *data = read_file(path, NULL);
    free(path);

    if (data) {
        char *save_ptr = NULL;
        char *line = strtok_r(data, "\n", &save_ptr);
        while (line) {
            char *trimmed = str_trim(line);
            if (trimmed[0] != '\0' && trimmed[0] != '#')
                cJSON_AddItemToArray(keys, cJSON_CreateString(trimmed));
            line = strtok_r(NULL, "\n", &save_ptr);
        }
        free(data);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "keys", keys);
    LOG_DEBUG("Retrieved %d SSH keys for user %s", cJSON_GetArraySize(keys), user_item->valuestring);
    return result;
}

static cJSON *handle_ssh_add_keys(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(args, "username");
    cJSON *keys_item = cJSON_GetObjectItemCaseSensitive(args, "keys");

    if (!cJSON_IsString(user_item) || !user_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'username' argument";
        return NULL;
    }
    if (!cJSON_IsArray(keys_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'keys' argument";
        return NULL;
    }

    struct passwd *pw = getpwnam(user_item->valuestring);
    if (!pw) {
        *err_class = "GenericError";
        *err_desc = "User not found";
        return NULL;
    }

    /* Create .ssh directory */
    char ssh_dir[512];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", pw->pw_dir);
    mkdir(ssh_dir, 0700);
    chown(ssh_dir, pw->pw_uid, pw->pw_gid);

    char *path = get_authorized_keys_path(user_item->valuestring);
    if (!path) {
        *err_class = "GenericError";
        *err_desc = "Failed to resolve authorized_keys path";
        return NULL;
    }

    /* Read existing keys */
    char *existing = read_file(path, NULL);

    /* Build new content with dedup */
    size_t cap = 4096;
    char *content = malloc(cap);
    size_t content_len = 0;
    if (!content) {
        free(existing);
        free(path);
        *err_class = "GenericError";
        *err_desc = "Memory allocation failed";
        return NULL;
    }
    content[0] = '\0';

    /* Copy existing keys */
    if (existing) {
        content_len = strlen(existing);
        if (content_len + 1 >= cap) {
            cap = content_len + 4096;
            content = realloc(content, cap);
        }
        memcpy(content, existing, content_len);
        content[content_len] = '\0';
    }

    /* Add new keys (skip duplicates) */
    cJSON *key_item;
    cJSON_ArrayForEach(key_item, keys_item) {
        if (!cJSON_IsString(key_item) || !key_item->valuestring)
            continue;
        /* Check for duplicate */
        if (existing && strstr(existing, key_item->valuestring))
            continue;

        size_t klen = strlen(key_item->valuestring);
        if (content_len + klen + 2 >= cap) {
            cap = content_len + klen + 4096;
            content = realloc(content, cap);
        }
        if (content_len > 0 && content[content_len - 1] != '\n')
            content[content_len++] = '\n';
        memcpy(content + content_len, key_item->valuestring, klen);
        content_len += klen;
        content[content_len++] = '\n';
        content[content_len] = '\0';
    }

    free(existing);

    if (write_file(path, content, content_len, 0600) != 0) {
        free(content);
        free(path);
        *err_class = "GenericError";
        *err_desc = "Failed to write authorized_keys";
        return NULL;
    }

    chown(path, pw->pw_uid, pw->pw_gid);
    free(content);
    free(path);

    LOG_DEBUG("Added SSH keys for user %s", user_item->valuestring);
    return cJSON_CreateObject();
}

static cJSON *handle_ssh_remove_keys(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(args, "username");
    cJSON *keys_item = cJSON_GetObjectItemCaseSensitive(args, "keys");

    if (!cJSON_IsString(user_item) || !user_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'username' argument";
        return NULL;
    }
    if (!cJSON_IsArray(keys_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'keys' argument";
        return NULL;
    }

    char *path = get_authorized_keys_path(user_item->valuestring);
    if (!path) {
        *err_class = "GenericError";
        *err_desc = "User not found";
        return NULL;
    }

    char *data = read_file(path, NULL);
    if (!data) {
        free(path);
        return cJSON_CreateObject(); /* No file = nothing to remove */
    }

    /* Filter out keys to remove */
    size_t cap = strlen(data) + 1;
    char *result_buf = malloc(cap);
    size_t result_len = 0;
    if (!result_buf) {
        free(data);
        free(path);
        *err_class = "GenericError";
        *err_desc = "Memory allocation failed";
        return NULL;
    }
    result_buf[0] = '\0';

    char *save_ptr = NULL;
    char *line = strtok_r(data, "\n", &save_ptr);
    while (line) {
        char *trimmed = str_trim(line);
        int should_remove = 0;

        cJSON *key_item;
        cJSON_ArrayForEach(key_item, keys_item) {
            if (cJSON_IsString(key_item) && key_item->valuestring &&
                strcmp(trimmed, key_item->valuestring) == 0) {
                should_remove = 1;
                break;
            }
        }

        if (!should_remove && trimmed[0] != '\0') {
            size_t tlen = strlen(trimmed);
            memcpy(result_buf + result_len, trimmed, tlen);
            result_len += tlen;
            result_buf[result_len++] = '\n';
            result_buf[result_len] = '\0';
        }

        line = strtok_r(NULL, "\n", &save_ptr);
    }

    free(data);

    struct passwd *pw = getpwnam(user_item->valuestring);
    write_file(path, result_buf, result_len, 0600);
    if (pw)
        chown(path, pw->pw_uid, pw->pw_gid);

    free(result_buf);
    free(path);

    LOG_DEBUG("Removed SSH keys for user %s", user_item->valuestring);
    return cJSON_CreateObject();
}

void cmd_ssh_init(void)
{
    command_register("guest-ssh-get-authorized-keys", handle_ssh_get_keys, 1);
    command_register("guest-ssh-add-authorized-keys", handle_ssh_add_keys, 1);
    command_register("guest-ssh-remove-authorized-keys", handle_ssh_remove_keys, 1);
}
