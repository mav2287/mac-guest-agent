#include "commands.h"
#include "compat.h"
#include "util.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_OPEN_FILES 64
#define READ_CHUNK_SIZE 49152  /* ~48KB, base64 expands to ~64KB */

typedef struct {
    int    fd;
    int    in_use;
    int64_t handle;
} open_file_t;

static open_file_t file_table[MAX_OPEN_FILES];
static int64_t next_handle = 1000;

static open_file_t *find_by_handle(int64_t handle)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (file_table[i].in_use && file_table[i].handle == handle)
            return &file_table[i];
    }
    return NULL;
}

static open_file_t *alloc_entry(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].in_use)
            return &file_table[i];
    }
    return NULL;
}

static cJSON *handle_file_open(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'path' argument";
        return NULL;
    }

    const char *mode_str = "r";
    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(args, "mode");
    if (cJSON_IsString(mode_item) && mode_item->valuestring)
        mode_str = mode_item->valuestring;

    int flags = O_RDONLY;
    if (strcmp(mode_str, "w") == 0)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode_str, "a") == 0)
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strcmp(mode_str, "r+") == 0)
        flags = O_RDWR;
    else if (strcmp(mode_str, "w+") == 0)
        flags = O_RDWR | O_CREAT | O_TRUNC;

    open_file_t *entry = alloc_entry();
    if (!entry) {
        *err_class = "GenericError";
        *err_desc = "Too many open files";
        return NULL;
    }

    int fd = open(path_item->valuestring, flags, 0644);
    if (fd < 0) {
        *err_class = "GenericError";
        *err_desc = strerror(errno);
        return NULL;
    }

    compat_cloexec(fd);

    entry->fd = fd;
    entry->handle = next_handle++;
    entry->in_use = 1;

    LOG_DEBUG("Opened file %s as handle %lld", path_item->valuestring, (long long)entry->handle);

    return cJSON_CreateNumber((double)entry->handle);
}

static cJSON *handle_file_close(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *handle_item = cJSON_GetObjectItemCaseSensitive(args, "handle");
    if (!cJSON_IsNumber(handle_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'handle' argument";
        return NULL;
    }

    int64_t handle = (int64_t)handle_item->valuedouble;
    open_file_t *entry = find_by_handle(handle);
    if (!entry) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid file handle";
        return NULL;
    }

    close(entry->fd);
    entry->in_use = 0;
    LOG_DEBUG("Closed file handle %lld", (long long)handle);
    return cJSON_CreateObject();
}

static cJSON *handle_file_read(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *handle_item = cJSON_GetObjectItemCaseSensitive(args, "handle");
    if (!cJSON_IsNumber(handle_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'handle' argument";
        return NULL;
    }

    int count = READ_CHUNK_SIZE;
    cJSON *count_item = cJSON_GetObjectItemCaseSensitive(args, "count");
    if (cJSON_IsNumber(count_item) && count_item->valuedouble > 0) {
        count = (int)count_item->valuedouble;
        if (count > READ_CHUNK_SIZE) count = READ_CHUNK_SIZE;
    }

    int64_t handle = (int64_t)handle_item->valuedouble;
    open_file_t *entry = find_by_handle(handle);
    if (!entry) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid file handle";
        return NULL;
    }

    unsigned char *buf = malloc((size_t)count);
    if (!buf) {
        *err_class = "GenericError";
        *err_desc = "Memory allocation failed";
        return NULL;
    }

    ssize_t n = read(entry->fd, buf, (size_t)count);
    if (n < 0) {
        free(buf);
        *err_class = "GenericError";
        *err_desc = strerror(errno);
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)n);
    cJSON_AddBoolToObject(result, "eof", n == 0);

    if (n > 0) {
        char *b64 = base64_encode(buf, (size_t)n);
        cJSON_AddStringToObject(result, "buf-b64", b64 ? b64 : "");
        free(b64);
    } else {
        cJSON_AddStringToObject(result, "buf-b64", "");
    }

    free(buf);
    return result;
}

static cJSON *handle_file_write(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *handle_item = cJSON_GetObjectItemCaseSensitive(args, "handle");
    cJSON *buf_item = cJSON_GetObjectItemCaseSensitive(args, "buf-b64");

    if (!cJSON_IsNumber(handle_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'handle' argument";
        return NULL;
    }
    if (!cJSON_IsString(buf_item) || !buf_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'buf-b64' argument";
        return NULL;
    }

    int64_t handle = (int64_t)handle_item->valuedouble;
    open_file_t *entry = find_by_handle(handle);
    if (!entry) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid file handle";
        return NULL;
    }

    size_t decoded_len;
    unsigned char *data = base64_decode(buf_item->valuestring, &decoded_len);
    if (!data) {
        *err_class = "GenericError";
        *err_desc = "Failed to decode base64 data";
        return NULL;
    }

    ssize_t written = write(entry->fd, data, decoded_len);
    free(data);

    if (written < 0) {
        *err_class = "GenericError";
        *err_desc = strerror(errno);
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)written);
    cJSON_AddBoolToObject(result, "eof", 0);
    return result;
}

static cJSON *handle_file_seek(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *handle_item = cJSON_GetObjectItemCaseSensitive(args, "handle");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(args, "offset");
    cJSON *whence_item = cJSON_GetObjectItemCaseSensitive(args, "whence");

    if (!cJSON_IsNumber(handle_item) || !cJSON_IsNumber(offset_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'handle' or 'offset' argument";
        return NULL;
    }

    int64_t handle = (int64_t)handle_item->valuedouble;
    open_file_t *entry = find_by_handle(handle);
    if (!entry) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid file handle";
        return NULL;
    }

    off_t offset = (off_t)offset_item->valuedouble;
    int whence = SEEK_SET;
    if (cJSON_IsNumber(whence_item)) {
        int w = (int)whence_item->valuedouble;
        if (w == 1) whence = SEEK_CUR;
        else if (w == 2) whence = SEEK_END;
    }

    off_t pos = lseek(entry->fd, offset, whence);
    if (pos < 0) {
        *err_class = "GenericError";
        *err_desc = strerror(errno);
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "position", (double)pos);
    cJSON_AddBoolToObject(result, "eof", 0);
    return result;
}

static cJSON *handle_file_flush(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *handle_item = cJSON_GetObjectItemCaseSensitive(args, "handle");
    if (!cJSON_IsNumber(handle_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'handle' argument";
        return NULL;
    }

    int64_t handle = (int64_t)handle_item->valuedouble;
    open_file_t *entry = find_by_handle(handle);
    if (!entry) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid file handle";
        return NULL;
    }

    if (fsync(entry->fd) != 0) {
        *err_class = "GenericError";
        *err_desc = strerror(errno);
        return NULL;
    }

    return cJSON_CreateObject();
}

void cmd_file_init(void)
{
    memset(file_table, 0, sizeof(file_table));
    command_register("guest-file-open", handle_file_open, 1);
    command_register("guest-file-close", handle_file_close, 1);
    command_register("guest-file-read", handle_file_read, 1);
    command_register("guest-file-write", handle_file_write, 1);
    command_register("guest-file-seek", handle_file_seek, 1);
    command_register("guest-file-flush", handle_file_flush, 1);
}
