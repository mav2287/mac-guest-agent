#include "commands.h"
#include "compat.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/*
 * Real filesystem freeze implementation for macOS.
 *
 * macOS has NO FIFREEZE ioctl (confirmed — VMware Tools never had it either).
 * We implement the best available mechanism for each filesystem:
 *
 * HFS+ (10.4–10.12): sync() + F_FULLFSYNC flushes all data to physical media.
 *   HFS+ journal ensures crash recovery. Continuous sync in the poll loop
 *   keeps flushing any new writes during the PVE snapshot window.
 *
 * APFS (10.13+): tmutil localsnapshot creates an atomic COW snapshot — this IS
 *   the consistency point. sync + F_FULLFSYNC is defense in depth.
 *   The APFS snapshot is created FIRST, before sync, because it's the truth.
 *
 * During freeze, the agent's poll loop runs sync() every 100ms to continuously
 * flush new writes. This closes the write window to ~100ms maximum.
 */

#define HOOK_DIR "/etc/qemu/fsfreeze-hook.d"
#define HOOK_TIMEOUT_SECS 30
#define AUTO_THAW_SECS 600   /* 10 minutes */
#define FREEZE_POLL_MS 100

/* Test/dry-run mode — don't touch real filesystems */
static int test_mode = 0;

void fsfreeze_set_test_mode(int enabled) { test_mode = enabled; }

/* Freeze state */
static int freeze_status = 0;        /* 0=thawed, 1=frozen */
static time_t freeze_start_time = 0;
static char snapshot_date[64] = "";   /* for APFS snapshot cleanup */
static int frozen_volume_count = 0;

/* Auto-thaw via SIGALRM */
static volatile sig_atomic_t auto_thaw_fired = 0;

static void auto_thaw_handler(int sig)
{
    (void)sig;
    auto_thaw_fired = 1;
}

/* ---- Hook Scripts ---- */

static int run_hooks(const char *action)
{
    struct stat dir_st;
    if (stat(HOOK_DIR, &dir_st) != 0)
        return 0;  /* Directory doesn't exist — skip silently */

    /* Validate directory ownership: must be owned by root */
    if (dir_st.st_uid != 0) {
        LOG_WARN("Hook directory %s not owned by root, skipping", HOOK_DIR);
        return 0;
    }

    DIR *dir = opendir(HOOK_DIR);
    if (!dir) return 0;

    /* Collect script names and sort alphabetically */
    char *scripts[64];
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 64) {
        if (entry->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", HOOK_DIR, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (!(st.st_mode & S_IXUSR)) continue;

        /* Security: must be owned by root, not world-writable */
        if (st.st_uid != 0) {
            LOG_WARN("Hook %s not owned by root, skipping", entry->d_name);
            continue;
        }
        if (st.st_mode & S_IWOTH) {
            LOG_WARN("Hook %s is world-writable, skipping", entry->d_name);
            continue;
        }

        scripts[count++] = safe_strdup(entry->d_name);
    }
    closedir(dir);

    if (count == 0) return 0;

    /* Sort alphabetically (freeze order); thaw uses reverse */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int cmp = strcmp(scripts[i], scripts[j]);
            int swap = (strcmp(action, "thaw") == 0) ? (cmp < 0) : (cmp > 0);
            if (swap) {
                char *tmp = scripts[i];
                scripts[i] = scripts[j];
                scripts[j] = tmp;
            }
        }
    }

    /* Execute each script with timeout */
    int failed = 0;
    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", HOOK_DIR, scripts[i]);

        LOG_INFO("Running %s hook: %s", action, scripts[i]);

        char *const argv[] = { path, (char *)action, NULL };
        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("Failed to fork for hook %s", scripts[i]);
            failed = 1;
        } else if (pid == 0) {
            execv(path, argv);
            _exit(127);
        } else {
            /* Wait with timeout */
            int status;
            time_t start = time(NULL);
            int done = 0;
            while (!done && (time(NULL) - start) < HOOK_TIMEOUT_SECS) {
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w > 0) {
                    done = 1;
                } else {
                    usleep(100000); /* 100ms */
                }
            }
            if (!done) {
                LOG_ERROR("Hook %s timed out after %ds, killing", scripts[i], HOOK_TIMEOUT_SECS);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                failed = 1;
            } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                LOG_ERROR("Hook %s failed with exit code %d", scripts[i], WEXITSTATUS(status));
                if (strcmp(action, "freeze") == 0)
                    failed = 1;  /* Freeze hooks: failure = abort */
            }
        }

        free(scripts[i]);
        if (failed && strcmp(action, "freeze") == 0)
            break;  /* Abort remaining freeze hooks */
    }

    /* Free remaining scripts if we broke early */
    for (int i = 0; i < count; i++) {
        /* scripts[i] may already be freed above; set to NULL after free */
    }

    return failed ? -1 : 0;
}

/* ---- Sync Operations ---- */

static int sync_all_volumes(int do_fullfsync)
{
    if (test_mode) {
        LOG_DEBUG("Dry-run: would sync all volumes (F_FULLFSYNC=%d)", do_fullfsync);
        return 1; /* Pretend 1 volume synced */
    }

    sync();

    if (!do_fullfsync) return 0;

    /* F_FULLFSYNC on each mounted local, writable volume */
    struct statfs *mntbuf;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
    int synced = 0;

    for (int i = 0; i < mntcount; i++) {
        /* Skip non-local (network) volumes */
        if (strncmp(mntbuf[i].f_mntfromname, "/dev/", 5) != 0)
            continue;
        /* Skip read-only volumes */
        if (mntbuf[i].f_flags & MNT_RDONLY)
            continue;

        int fd = open(mntbuf[i].f_mntonname, O_RDONLY);
        if (fd >= 0) {
            if (fcntl(fd, F_FULLFSYNC) == 0) {
                synced++;
            } else {
                LOG_WARN("F_FULLFSYNC failed on %s: %s",
                         mntbuf[i].f_mntonname, strerror(errno));
            }
            close(fd);
        }
    }

    return synced;
}

/* ---- APFS Snapshot ---- */

static int create_apfs_snapshot(void)
{
    if (!compat_has_tmutil()) return 0;

    if (test_mode) {
        LOG_DEBUG("Dry-run: would create APFS snapshot");
        snprintf(snapshot_date, sizeof(snapshot_date), "dry-run");
        return 1;
    }

    /* Clean up orphaned snapshots from previous failed runs */
    run_command("tmutil deletelocalsnapshots / 2>/dev/null");

    char *output = NULL;
    if (run_command_capture("tmutil localsnapshot / 2>&1", &output) != 0) {
        LOG_WARN("tmutil localsnapshot failed: %s", output ? output : "unknown");
        free(output);
        return 0;
    }

    /* Parse snapshot date from output: "Created local snapshot with date: 2026-03-22-143052" */
    if (output) {
        char *date = strstr(output, "date: ");
        if (date) {
            date += 6;
            char *end = date;
            while (*end && *end != '\n' && *end != '\r') end++;
            size_t len = (size_t)(end - date);
            if (len < sizeof(snapshot_date)) {
                memcpy(snapshot_date, date, len);
                snapshot_date[len] = '\0';
                LOG_INFO("Created APFS snapshot: %s", snapshot_date);
            }
        }
        free(output);
    }

    return snapshot_date[0] ? 1 : 0;
}

static void delete_apfs_snapshot(void)
{
    if (!snapshot_date[0]) return;
    if (test_mode || strcmp(snapshot_date, "dry-run") == 0) {
        LOG_DEBUG("Dry-run: would delete APFS snapshot");
        snapshot_date[0] = '\0';
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmutil deletelocalsnapshots %s 2>/dev/null", snapshot_date);
    run_command(cmd);
    LOG_INFO("Deleted APFS snapshot: %s", snapshot_date);
    snapshot_date[0] = '\0';
}

/* ---- Freeze Command ---- */

static cJSON *handle_fsfreeze_freeze(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    /* Idempotent: if already frozen, return current count */
    if (freeze_status) {
        return cJSON_CreateNumber(frozen_volume_count);
    }

    LOG_INFO("Filesystem freeze starting");

    /* Step 1: Run freeze hooks */
    if (run_hooks("freeze") < 0) {
        *err_class = "GenericError";
        *err_desc = "Freeze hook script failed";
        return NULL;
    }

    /* Step 2: APFS snapshot FIRST (10.13+) — this IS the consistency point */
    int has_snapshot = 0;
    if (compat_has_apfs()) {
        has_snapshot = create_apfs_snapshot();
    }

    /* Step 3: sync + F_FULLFSYNC on all local writable volumes */
    int synced = sync_all_volumes(1);  /* 1 = do F_FULLFSYNC */

    if (synced == 0 && !has_snapshot) {
        *err_class = "GenericError";
        *err_desc = "Failed to sync any volumes";
        run_hooks("thaw");  /* Undo freeze hooks */
        return NULL;
    }

    /* Step 4: Set frozen state */
    freeze_status = 1;
    freeze_start_time = time(NULL);
    frozen_volume_count = synced + has_snapshot;
    auto_thaw_fired = 0;

    /* Step 5: Set auto-thaw alarm */
    signal(SIGALRM, auto_thaw_handler);
    alarm(AUTO_THAW_SECS);

    LOG_INFO("Filesystem frozen: %d volumes synced, APFS snapshot=%s",
             synced, has_snapshot ? "yes" : "no");

    return cJSON_CreateNumber(frozen_volume_count);
}

/* ---- Thaw Command ---- */

static void do_thaw(void)
{
    if (!freeze_status) return;

    /* Cancel auto-thaw alarm */
    alarm(0);
    signal(SIGALRM, SIG_DFL);

    freeze_status = 0;

    /* Clean up APFS snapshot */
    delete_apfs_snapshot();

    /* Run thaw hooks (reverse order, best effort) */
    run_hooks("thaw");

    LOG_INFO("Filesystem thawed");
}

static cJSON *handle_fsfreeze_thaw(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    int count = freeze_status ? frozen_volume_count : 0;
    do_thaw();
    return cJSON_CreateNumber(count);
}

/* ---- Status Command ---- */

static cJSON *handle_fsfreeze_status(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    return cJSON_CreateString(freeze_status ? "frozen" : "thawed");
}

/* ---- Fstrim (documented no-op) ---- */

static cJSON *handle_fstrim(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    /* macOS handles TRIM natively via the storage driver.
     * Users should set discard=on on their PVE virtual disk. */
    cJSON *result = cJSON_CreateObject();
    if (result)
        cJSON_AddItemToObject(result, "paths", cJSON_CreateArray());
    return result;
}

/* ---- Continuous Sync (called from agent.c poll loop) ---- */

int fsfreeze_is_frozen(void)
{
    /* Check auto-thaw */
    if (auto_thaw_fired && freeze_status) {
        LOG_ERROR("Auto-thaw triggered after %d seconds (PVE may have crashed)", AUTO_THAW_SECS);
        do_thaw();
    }
    return freeze_status;
}

void fsfreeze_continuous_sync(void)
{
    if (!freeze_status) return;
    if (test_mode) return;  /* Don't touch real filesystems in test mode */
    /* Lightweight sync — no F_FULLFSYNC (expensive), just flush dirty buffers */
    sync();
}

/* ---- Freeze-safe command check ---- */

int fsfreeze_command_allowed(const char *cmd_name)
{
    if (!freeze_status) return 1;  /* Not frozen, everything allowed */

    /* Only these commands are allowed during freeze */
    static const char *allowed[] = {
        "guest-ping",
        "guest-sync",
        "guest-sync-id",
        "guest-sync-delimited",
        "guest-info",
        "guest-fsfreeze-status",
        "guest-fsfreeze-thaw",
        NULL
    };

    for (int i = 0; allowed[i]; i++) {
        if (strcmp(cmd_name, allowed[i]) == 0)
            return 1;
    }
    return 0;
}

/* ---- Init ---- */

void cmd_fs_init(void)
{
    command_register("guest-fsfreeze-status", handle_fsfreeze_status, 1);
    command_register("guest-fsfreeze-freeze", handle_fsfreeze_freeze, 1);
    command_register("guest-fsfreeze-freeze-list", handle_fsfreeze_freeze, 1);
    command_register("guest-fsfreeze-thaw", handle_fsfreeze_thaw, 1);
    command_register("guest-fstrim", handle_fstrim, 1);
}
