#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>

/* Read a file via O_NOFOLLOW + fstat — refuses to open a symlink at the
 * final path component, which is the privesc surface for SSH paths run
 * as root (an attacker who owns the home dir can swap a symlink in for
 * .ssh/authorized_keys and trick a root-running agent into reading a
 * root-owned file like /etc/shadow).
 *
 * Returns a heap-allocated NUL-terminated buffer (caller frees) and
 * sets *out_len, OR NULL on any error including:
 *  - path is a symlink (open returns ELOOP with O_NOFOLLOW)
 *  - path is not a regular file (fstat S_ISREG check)
 *  - open failed for any other reason (ENOENT, EACCES, etc.)
 *
 * O_NOFOLLOW is POSIX.1-2001 and present on every macOS from 10.0
 * onwards — works on the i386/10.4 build. */
/* Exposed (not static) for unit testing — regression tests live in
 * tests/test_proactive.c. */
char *ssh_safe_read_file(const char *path, size_t *out_len);
char *ssh_safe_read_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX - 1) {
        close(fd);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) { close(fd); return NULL; }

    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return NULL;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

/* Atomically write `data` to `path` as a regular file owned by `uid:gid`
 * with mode 0600.
 *
 * Pattern: write to a per-pid temp file in the SAME directory as the
 * target, fchown/fchmod by fd, then rename(2) over the target.
 * rename() is POSIX-atomic for same-directory same-filesystem swaps,
 * so readers (sshd checking authorized_keys for the next auth, a
 * backup utility, etc.) NEVER see a partial-write state. If the agent
 * crashes between create-temp and rename, the target's prior state
 * (file content or absence) is preserved — the only leftover is a
 * `.<basename>.tmp.<pid>` dot-file the operator can clean up.
 *
 * Also closes the second TOCTOU window left by the prior open-truncate
 * approach: rename() replaces the path entry atomically. If an
 * attacker pre-positioned a symlink at `path` they would have their
 * symlink atomically swapped for our newly-created regular file — not
 * followed.
 *
 * Tiger-compatible: every primitive (O_WRONLY/O_CREAT/O_EXCL/
 * O_NOFOLLOW for open, fchown, fchmod, write, close, rename, unlink)
 * is POSIX.1-2001 or older, and present on macOS since 10.0.
 * Deliberately doesn't use openat()/renameat() (POSIX.1-2008, macOS
 * 10.5+) — those would lock the parent directory too, but the
 * marginal value is small here because the parent directory was
 * already validated by ssh_safe_ssh_dir() and the target filename
 * is fully derived from getpwnam() output. i386/10.4 build clean.
 *
 * Returns 0 on success, -1 on any error.
 *
 * Exposed (not static) for unit testing in tests/test_proactive.c. */
int ssh_safe_write_file(const char *path, uid_t uid, gid_t gid,
                         const char *data, size_t len);
int ssh_safe_write_file(const char *path, uid_t uid, gid_t gid,
                         const char *data, size_t len)
{
    /* Build the temp path in the same directory as the target. Same-
     * directory is required for rename() atomicity (cross-FS rename
     * isn't guaranteed atomic). For a target like
     *   /Users/foo/.ssh/authorized_keys
     * the temp ends up at
     *   /Users/foo/.ssh/.authorized_keys.tmp.<pid> */
    char tmp_path[PATH_MAX];
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    int needed;
    if (slash) {
        needed = snprintf(tmp_path, sizeof(tmp_path), "%.*s.%s.tmp.%d",
                          (int)(slash - path + 1), path, base, (int)getpid());
    } else {
        needed = snprintf(tmp_path, sizeof(tmp_path), ".%s.tmp.%d",
                          base, (int)getpid());
    }
    if (needed < 0 || (size_t)needed >= sizeof(tmp_path)) {
        LOG_WARN("ssh_safe_write_file(%s): path too long for temp file",
                 path);
        return -1;
    }

    /* O_EXCL guarantees we create a fresh file — refuses if a temp
     * file with this name already exists (stale from a prior crash;
     * operator can clean it up, we error out). O_NOFOLLOW protects
     * the temp path itself from a pre-positioned symlink (defence in
     * depth — should not happen on a properly-permissioned .ssh dir
     * but costs nothing to add). */
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        LOG_WARN("ssh_safe_write_file: temp file open(%s) failed: %s",
                 tmp_path, strerror(errno));
        return -1;
    }

    /* fchown/fchmod by fd, not by path — even if some other process
     * races us on the temp path, our metadata changes apply to OUR
     * fd's inode. */
    if (fchown(fd, uid, gid) < 0) {
        LOG_WARN("ssh_safe_write_file(%s): fchown failed: %s",
                 tmp_path, strerror(errno));
        /* Don't bail — the rename below still happens; the file just
         * stays as whatever uid:gid open() created it under. Mirror
         * previous error-tolerant behaviour. */
    }
    if (fchmod(fd, 0600) < 0) {
        LOG_WARN("ssh_safe_write_file(%s): fchmod failed: %s",
                 tmp_path, strerror(errno));
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp_path);            /* clean up the partial temp */
            return -1;
        }
        written += (size_t)n;
    }
    if (close(fd) < 0) {
        LOG_WARN("ssh_safe_write_file(%s): close failed: %s",
                 tmp_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    /* The atomic step. rename(2) over an existing entry atomically
     * replaces it — regular file, symlink, anything. POSIX guarantee
     * for same-directory same-filesystem operations. */
    if (rename(tmp_path, path) < 0) {
        LOG_WARN("ssh_safe_write_file: rename(%s -> %s) failed: %s",
                 tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

/* Create (or verify) the user's .ssh directory safely.
 *
 *  - mkdir() doesn't follow symlinks for the final component, so a
 *    pre-existing symlink at the .ssh path returns EEXIST. We lstat
 *    to verify what's there is actually a directory (not a symlink).
 *  - lchown() instead of chown() to set ownership without following
 *    symlinks (defence in depth — we already verified it's a real
 *    directory, but the lchown is one less attack surface).
 *
 * Returns 0 on success, -1 if .ssh is something other than a real
 * directory we can put a file under. Logs the specific failure. */
static int ssh_safe_ssh_dir(const char *home, uid_t uid, gid_t gid, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/.ssh", home);
    if (mkdir(out, 0700) < 0 && errno != EEXIST) {
        LOG_WARN("ssh_safe_ssh_dir(%s): mkdir failed: %s",
                 out, strerror(errno));
        return -1;
    }
    struct stat st;
    if (lstat(out, &st) < 0) {
        LOG_WARN("ssh_safe_ssh_dir(%s): lstat failed: %s",
                 out, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        LOG_WARN("ssh_safe_ssh_dir(%s): not a regular directory "
                 "(mode=0%o) — refusing to operate on it",
                 out, (unsigned)st.st_mode);
        return -1;
    }
    if (lchown(out, uid, gid) < 0) {
        LOG_WARN("ssh_safe_ssh_dir(%s): lchown failed: %s",
                 out, strerror(errno));
    }
    return 0;
}

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
    /* O_NOFOLLOW read — refuse to follow a symlink at authorized_keys
     * even for the read path. An attacker who owns the home dir could
     * point the symlink at /etc/shadow and trick the root-running
     * agent into exposing the content via the QGA response. */
    char *data = ssh_safe_read_file(path, NULL);
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

    /* Create or verify .ssh directory safely (refuses to operate if
     * .ssh is a symlink — see ssh_safe_ssh_dir). */
    char ssh_dir[512];
    if (ssh_safe_ssh_dir(pw->pw_dir, pw->pw_uid, pw->pw_gid, ssh_dir, sizeof(ssh_dir)) < 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to create or verify .ssh directory";
        return NULL;
    }

    char *path = get_authorized_keys_path(user_item->valuestring);
    if (!path) {
        *err_class = "GenericError";
        *err_desc = "Failed to resolve authorized_keys path";
        return NULL;
    }

    /* Read existing keys via the O_NOFOLLOW safe reader (symlink at
     * authorized_keys is rejected — keeps the same fchown-by-fd safety pattern documented in CHANGELOG v2.4.3). */
    char *existing = ssh_safe_read_file(path, NULL);

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
            char *tmp = realloc(content, cap);
            if (!tmp) { free(content); free(existing); free(path);
                *err_class = "GenericError"; *err_desc = "Out of memory"; return NULL; }
            content = tmp;
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
            char *tmp = realloc(content, cap);
            if (!tmp) { free(content); free(existing); free(path);
                *err_class = "GenericError"; *err_desc = "Out of memory"; return NULL; }
            content = tmp;
        }
        if (content_len > 0 && content[content_len - 1] != '\n')
            content[content_len++] = '\n';
        memcpy(content + content_len, key_item->valuestring, klen);
        content_len += klen;
        content[content_len++] = '\n';
        content[content_len] = '\0';
    }

    free(existing);

    /* Atomic-ish safe write: O_NOFOLLOW prevents symlink targets,
     * fchown/fchmod by fd binds the metadata change to OUR fd's
     * inode regardless of any racing path swap. */
    if (ssh_safe_write_file(path, pw->pw_uid, pw->pw_gid, content, content_len) != 0) {
        free(content);
        free(path);
        *err_class = "GenericError";
        *err_desc = "Failed to write authorized_keys (temp create or rename failed; .ssh directory may have wrong permissions)";
        return NULL;
    }

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

    /* O_NOFOLLOW read — symlink at authorized_keys is rejected
     * before we expose its contents. */
    char *data = ssh_safe_read_file(path, NULL);
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
    if (!pw) {
        free(result_buf);
        free(path);
        *err_class = "GenericError";
        *err_desc = "User not found";
        return NULL;
    }
    /* Same safe-write helper as the add path — O_NOFOLLOW + fchown/
     * fchmod by fd. */
    if (ssh_safe_write_file(path, pw->pw_uid, pw->pw_gid, result_buf, result_len) != 0) {
        LOG_ERROR("Failed to write authorized_keys during remove (temp create or rename failed)");
        free(result_buf);
        free(path);
        *err_class = "GenericError";
        *err_desc = "Failed to write authorized_keys file";
        return NULL;
    }

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
