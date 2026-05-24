#include "cmd-fs.h"
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
        scripts[i] = NULL;
        if (failed && strcmp(action, "freeze") == 0)
            break;  /* Abort remaining freeze hooks */
    }

    /* Free remaining scripts if we broke early */
    for (int i = 0; i < count; i++) {
        free(scripts[i]);  /* free(NULL) is a no-op for already-freed entries */
    }

    return failed ? -1 : 0;
}

/* ---- Per-filesystem-type dispatch ----
 *
 * Replaces the previous uniform "F_FULLFSYNC on every writable /dev-backed
 * mount" loop with a per-FS-type strategy. macOS has no FIFREEZE, so we get
 * exactly what the platform provides per filesystem:
 *
 *   APFS  - tmutil localsnapshot (real atomic consistency point) at the
 *           container level, plus F_FULLFSYNC per mount as defence in depth.
 *           The snapshot is taken by handle_fsfreeze_freeze before this
 *           loop runs; we only do the per-mount F_FULLFSYNC here.
 *   HFS+  - F_FULLFSYNC (best-effort flush to media).
 *   FAT / exFAT / UDF / NTFS R/W / unknown - try F_FULLFSYNC, tolerate
 *           ENOTSUP (older drivers don't implement it; the global sync()
 *           at the top already flushed dirty buffers).
 *   ZFS   - Phase 2 item 3 will add `zfs snapshot`; for this commit treat
 *           ZFS the same as GENERIC_WRITABLE.
 *   Network (smbfs/afpfs/nfs/webdav/ftp) - skip; not the guest's concern.
 *   Special (devfs/fdesc/volfs/synthfs/lifs/autofs) or non-/dev-backed -
 *           skip; no meaningful flush semantics.
 *   Read-only - skip; nothing to flush.
 *
 * See docs/design/AGENT_BEHAVIOUR_SPEC.md Q1 for the full design.
 */

fs_class_t fs_dispatch_class(const struct statfs *mnt)
{
    if (!mnt) return FS_SKIP_SPECIAL;

    if (mnt->f_flags & MNT_RDONLY) return FS_SKIP_RDONLY;

    const char *type = mnt->f_fstypename;

    if (strcmp(type, "smbfs") == 0 ||
        strcmp(type, "afpfs") == 0 ||
        strcmp(type, "nfs") == 0 ||
        strcmp(type, "webdav") == 0 ||
        strcmp(type, "ftp") == 0) {
        return FS_SKIP_NETWORK;
    }

    if (strcmp(type, "devfs") == 0 ||
        strcmp(type, "fdesc") == 0 ||
        strcmp(type, "volfs") == 0 ||
        strcmp(type, "synthfs") == 0 ||
        strcmp(type, "lifs") == 0 ||
        strcmp(type, "autofs") == 0) {
        return FS_SKIP_SPECIAL;
    }

    /* Known writable types — type-based, not /dev/-backing-based. ZFS in
     * particular doesn't use /dev/ in f_mntfromname (it's pool/dataset),
     * so the /dev/ check below must come AFTER this. */
    if (strcmp(type, "apfs") == 0) return FS_APFS_WRITABLE;
    if (strcmp(type, "zfs") == 0)  return FS_ZFS_WRITABLE;
    if (strcmp(type, "hfs") == 0)  return FS_HFS_WRITABLE;

    /* Unknown writable type — only proceed if /dev/-backed; otherwise
     * be defensive (FUSE drivers, exotic non-device mounts). */
    if (strncmp(mnt->f_mntfromname, "/dev/", 5) != 0) {
        return FS_SKIP_SPECIAL;
    }

    return FS_GENERIC_WRITABLE;
}

/* Forward declarations — implementations live in the ZFS Snapshot section
 * after handle_fsfreeze_freeze. Declared here so sync_all_volumes (just
 * below) can dispatch the FS_ZFS_WRITABLE case. */
static int  create_zfs_snapshot(const struct statfs *mnt, fs_counts_t *c);
static void delete_zfs_snapshots(void);

/* Per-mount F_FULLFSYNC with ENOTSUP/EOPNOTSUPP tolerance.
 * Increments the supplied counters. */
static void try_fullfsync(const struct statfs *mnt, fs_counts_t *c)
{
    int fd = open(mnt->f_mntonname, O_RDONLY);
    if (fd < 0) {
        LOG_WARN("Cannot open %s (%s) for sync: %s",
                 mnt->f_mntonname, mnt->f_fstypename, strerror(errno));
        /* The global sync() at the top of sync_all_volumes already flushed
         * dirty buffers; count this as flushed_only rather than losing it. */
        c->flushed_only++;
        return;
    }

    if (fcntl(fd, F_FULLFSYNC) == 0) {
        LOG_DEBUG("F_FULLFSYNC ok on %s (%s)",
                  mnt->f_mntonname, mnt->f_fstypename);
        c->fullfsynced++;
    } else if (errno == ENOTSUP || errno == EOPNOTSUPP) {
        /* By design on filesystems that don't implement F_FULLFSYNC
         * (older MS-DOS driver on Tiger-era macOS, third-party FUSE
         * drivers, etc.). The global sync() already flushed dirty
         * buffers — count as flushed_only at DEBUG, not WARN. */
        LOG_DEBUG("F_FULLFSYNC unsupported on %s (%s) — flushed via sync() only",
                  mnt->f_mntonname, mnt->f_fstypename);
        c->flushed_only++;
    } else {
        /* Unexpected error (EIO, EAGAIN, EACCES, etc.). Real problem —
         * surface it. Still count as flushed_only — sync() covered the data. */
        LOG_WARN("F_FULLFSYNC failed on %s (%s): %s",
                 mnt->f_mntonname, mnt->f_fstypename, strerror(errno));
        c->flushed_only++;
    }

    close(fd);
}

/* Return non-zero if `mountpoint` matches any entry in the allowlist. */
static int mountpoint_in_list(const char *mountpoint,
                              const char *const *list, int list_len)
{
    if (!list || list_len <= 0) return 1;  /* no filter — match all */
    for (int j = 0; j < list_len; j++) {
        if (list[j] && strcmp(mountpoint, list[j]) == 0) return 1;
    }
    return 0;
}

/* ---- Sync Operations ----
 *
 * Caller initialises `counts` (typically by setting snapshotted /
 * zfs_snapshotted to reflect APFS / ZFS snapshot outcomes captured
 * elsewhere, then zeroing the rest). sync_all_volumes only INCREMENTS
 * counts as it iterates — it does not reset them — so any snapshot
 * results captured by the caller before invocation are preserved.
 *
 * `mountpoints` (optional, may be NULL): if non-NULL and `n_mountpoints>0`,
 * only mounts whose `f_mntonname` matches one of the listed paths are
 * processed. Used by handle_fsfreeze_freeze_list to implement the QGA
 * spec's subset-freeze behaviour. NULL/0 means "process all writable
 * local mounts that pass fs_dispatch_class" — the default global-freeze
 * behaviour.
 *
 * Returns the total of "did-something" counters
 * (snapshotted + zfs_snapshotted + fullfsynced + flushed_only) for the
 * caller's wire response.
 */
static int sync_all_volumes(int do_fullfsync, fs_counts_t *counts,
                            const char *const *mountpoints, int n_mountpoints)
{
    if (test_mode) {
        LOG_DEBUG("Dry-run: would sync all volumes (F_FULLFSYNC=%d, filter=%d)",
                  do_fullfsync, n_mountpoints);
        /* Honour the filter count so integration tests can verify the
         * parameter plumbed through: pretend every listed mountpoint
         * succeeded (subset freeze), or one volume succeeded (global). */
        counts->fullfsynced += (n_mountpoints > 0) ? n_mountpoints : 1;
        return counts->snapshotted + counts->zfs_snapshotted +
               counts->fullfsynced + counts->flushed_only;
    }

    sync();

    if (!do_fullfsync) {
        return counts->snapshotted + counts->zfs_snapshotted +
               counts->fullfsynced + counts->flushed_only;
    }

    struct statfs *mntbuf;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    for (int i = 0; i < mntcount; i++) {
        const char *mp = mntbuf[i].f_mntonname;
        const char *type = mntbuf[i].f_fstypename;

        /* Subset-freeze filter (Q2 — guest-fsfreeze-freeze-list): only
         * process mounts in the supplied allowlist. NULL/0 means "all". */
        if (!mountpoint_in_list(mp, mountpoints, n_mountpoints)) {
            continue;
        }

        fs_class_t cls = fs_dispatch_class(&mntbuf[i]);

        switch (cls) {
        case FS_SKIP_RDONLY:
            LOG_DEBUG("Skip %s (%s): read-only", mp, type);
            counts->skipped_readonly++;
            break;
        case FS_SKIP_NETWORK:
            LOG_DEBUG("Skip %s (%s): network mount", mp, type);
            counts->skipped_network++;
            break;
        case FS_SKIP_SPECIAL:
            LOG_DEBUG("Skip %s (%s): special filesystem", mp, type);
            counts->skipped_special++;
            break;
        case FS_APFS_WRITABLE:
        case FS_HFS_WRITABLE:
        case FS_GENERIC_WRITABLE:
            try_fullfsync(&mntbuf[i], counts);
            break;
        case FS_ZFS_WRITABLE:
            /* Prefer `zfs snapshot` — the real atomic consistency primitive
             * for ZFS. Falls through to F_FULLFSYNC as defence in depth
             * if the `zfs` CLI is absent or the snapshot fails. */
            if (!create_zfs_snapshot(&mntbuf[i], counts)) {
                try_fullfsync(&mntbuf[i], counts);
            }
            break;
        }
    }

    return counts->snapshotted + counts->zfs_snapshotted +
           counts->fullfsynced + counts->flushed_only;
}

/* ---- APFS Snapshot ---- */

static void delete_apfs_snapshot(void);

static int create_apfs_snapshot(void)
{
    if (!compat_has_tmutil()) return 0;

    if (test_mode) {
        LOG_DEBUG("Dry-run: would create APFS snapshot");
        snprintf(snapshot_date, sizeof(snapshot_date), "dry-run");
        return 1;
    }

    /* If there's an orphaned snapshot from a previous failed run, clean it up.
     * Only delete the specific snapshot we tracked, not all Time Machine snapshots. */
    if (snapshot_date[0]) {
        delete_apfs_snapshot();
    }

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

    char *const argv[] = {"tmutil", "deletelocalsnapshots", snapshot_date, NULL};
    run_command_v("/usr/bin/tmutil", argv, NULL, NULL);
    LOG_INFO("Deleted APFS snapshot: %s", snapshot_date);
    snapshot_date[0] = '\0';
}

/* ---- ZFS Snapshot (third-party OpenZFS-on-macOS) ----
 *
 * ZFS provides a real atomic snapshot primitive (`zfs snapshot`), so for
 * ZFS-typed mounts we prefer it over F_FULLFSYNC (which isn't documented
 * to be supported on ZFS and would give a weaker consistency guarantee
 * anyway). One snapshot per dataset; tracked so the matching thaw can
 * `zfs destroy` them.
 *
 * If the `zfs` CLI is not present on the guest (OpenZFS-on-macOS is a
 * third-party install), the dispatch falls through to try_fullfsync —
 * same as any other unknown writable type.
 */

#define MAX_ZFS_SNAPSHOTS 16
static char zfs_snapshot_names[MAX_ZFS_SNAPSHOTS][256];
static int  zfs_snapshot_count = 0;

/* Cached CLI lookup. -1 = not yet checked, 0 = absent, 1 = present. */
static int  zfs_cli_state = -1;
static char zfs_cli_path[64] = "";

static const char *find_zfs_cli(void)
{
    if (zfs_cli_state == 1) return zfs_cli_path;
    if (zfs_cli_state == 0) return NULL;

    static const char *candidates[] = {
        "/usr/local/sbin/zfs",   /* OpenZFS-on-macOS default */
        "/usr/local/bin/zfs",
        "/opt/local/bin/zfs",    /* MacPorts */
        "/opt/homebrew/bin/zfs", /* Apple Silicon Homebrew */
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            strncpy(zfs_cli_path, candidates[i], sizeof(zfs_cli_path) - 1);
            zfs_cli_path[sizeof(zfs_cli_path) - 1] = '\0';
            zfs_cli_state = 1;
            return zfs_cli_path;
        }
    }
    zfs_cli_state = 0;
    return NULL;
}

/* Snapshot a single ZFS dataset. f_mntfromname for a ZFS mount is the
 * dataset name (e.g. "tank/data"), not a /dev/... path. Returns 1 on
 * success, 0 on failure or if the CLI isn't available; on failure the
 * caller falls through to try_fullfsync as defence in depth. */
static int create_zfs_snapshot(const struct statfs *mnt, fs_counts_t *c)
{
    const char *cli = find_zfs_cli();
    if (!cli) return 0;
    if (zfs_snapshot_count >= MAX_ZFS_SNAPSHOTS) {
        LOG_WARN("ZFS snapshot tracking full (%d entries); skipping snapshot for %s",
                 MAX_ZFS_SNAPSHOTS, mnt->f_mntonname);
        return 0;
    }

    char snap[256];
    snprintf(snap, sizeof(snap), "%s@mac-guest-agent-%lld",
             mnt->f_mntfromname, (long long)time(NULL));

    if (test_mode) {
        LOG_DEBUG("Dry-run: would `zfs snapshot %s`", snap);
        strncpy(zfs_snapshot_names[zfs_snapshot_count], snap,
                sizeof(zfs_snapshot_names[0]) - 1);
        zfs_snapshot_names[zfs_snapshot_count][sizeof(zfs_snapshot_names[0]) - 1] = '\0';
        zfs_snapshot_count++;
        c->zfs_snapshotted++;
        return 1;
    }

    char *const argv[] = { (char *)cli, "snapshot", snap, NULL };
    if (run_command_v(cli, argv, NULL, NULL) != 0) {
        LOG_WARN("`zfs snapshot %s` failed; will fall through to F_FULLFSYNC", snap);
        return 0;
    }

    LOG_INFO("Created ZFS snapshot: %s", snap);
    strncpy(zfs_snapshot_names[zfs_snapshot_count], snap,
            sizeof(zfs_snapshot_names[0]) - 1);
    zfs_snapshot_names[zfs_snapshot_count][sizeof(zfs_snapshot_names[0]) - 1] = '\0';
    zfs_snapshot_count++;
    c->zfs_snapshotted++;
    return 1;
}

/* Destroy every tracked ZFS snapshot. Called from do_thaw. Failure on
 * any one snapshot is logged WARN but doesn't stop the cleanup loop —
 * we want a best-effort destroy of as many as possible. */
static void delete_zfs_snapshots(void)
{
    if (zfs_snapshot_count == 0) return;
    const char *cli = find_zfs_cli();

    for (int i = 0; i < zfs_snapshot_count; i++) {
        if (test_mode) {
            LOG_DEBUG("Dry-run: would `zfs destroy %s`", zfs_snapshot_names[i]);
            continue;
        }
        if (!cli) {
            /* Shouldn't happen — we only get here if we successfully created
             * snapshots, which requires the CLI. Defensive: skip cleanly. */
            LOG_WARN("ZFS CLI no longer found; leaving snapshot %s orphaned",
                     zfs_snapshot_names[i]);
            continue;
        }
        char *const argv[] = { (char *)cli, "destroy", zfs_snapshot_names[i], NULL };
        if (run_command_v(cli, argv, NULL, NULL) != 0) {
            LOG_WARN("`zfs destroy %s` failed", zfs_snapshot_names[i]);
        } else {
            LOG_INFO("Deleted ZFS snapshot: %s", zfs_snapshot_names[i]);
        }
    }
    zfs_snapshot_count = 0;
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
    fs_counts_t counts;
    memset(&counts, 0, sizeof(counts));

    int has_snapshot = 0;
    if (compat_has_apfs()) {
        has_snapshot = create_apfs_snapshot();
    }
    counts.snapshotted = has_snapshot;

    /* Step 3: sync + per-FS dispatch on writable local volumes.
     * sync_all_volumes() increments counts; it does not reset them, so
     * the snapshotted value set above is preserved in the totals.
     * NULL mountpoint filter = process all mounts (global freeze). */
    int total = sync_all_volumes(1, &counts, NULL, 0);

    if (total == 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to sync any volumes";
        run_hooks("thaw");  /* Undo freeze hooks */
        return NULL;
    }

    /* Step 4: Set frozen state */
    freeze_status = 1;
    freeze_start_time = time(NULL);
    frozen_volume_count = total;
    auto_thaw_fired = 0;

    /* Step 5: Set auto-thaw alarm */
    signal(SIGALRM, auto_thaw_handler);
    alarm(AUTO_THAW_SECS);

    /* Single INFO line capturing the full per-treatment breakdown.
     * This is what scripts/verify.sh greps for to report the per-FS
     * outcome from the host side (Phase 3 work). */
    int skipped = counts.skipped_network + counts.skipped_special +
                  counts.skipped_readonly;
    LOG_INFO("Filesystem frozen: %d snapshotted, %d zfs_snapshotted, "
             "%d fullfsynced, %d flushed_only (=%d total); "
             "skipped %d (%d network, %d special, %d readonly)",
             counts.snapshotted, counts.zfs_snapshotted,
             counts.fullfsynced, counts.flushed_only, total,
             skipped,
             counts.skipped_network, counts.skipped_special, counts.skipped_readonly);

    return cJSON_CreateNumber(frozen_volume_count);
}

/* QGA spec: guest-fsfreeze-freeze-list takes optional `mountpoints: [str]`.
 * If absent / empty array, behave like guest-fsfreeze-freeze (freeze all).
 * If present and non-empty, freeze ONLY the listed mountpoints; leave the
 * rest writable. Typical use: a backup tool freezes a data volume but
 * keeps the OS volume writable for its own logs.
 *
 * For subset freezes we intentionally skip the container-level APFS
 * `tmutil localsnapshot` — that snapshot is per-container, not per-mount,
 * so taking it for a subset request would capture state the caller didn't
 * ask us to capture. Per-mount F_FULLFSYNC is the consistency mechanism
 * for subset freezes. */
static cJSON *handle_fsfreeze_freeze_list(cJSON *args,
                                          const char **err_class,
                                          const char **err_desc)
{
    cJSON *mountpoints_node = args ? cJSON_GetObjectItem(args, "mountpoints") : NULL;

    /* No mountpoints argument, or empty array: same as guest-fsfreeze-freeze. */
    if (!mountpoints_node || !cJSON_IsArray(mountpoints_node) ||
        cJSON_GetArraySize(mountpoints_node) == 0) {
        return handle_fsfreeze_freeze(args, err_class, err_desc);
    }

    /* Validate every entry is a string and build a C array of borrowed
     * pointers into the cJSON tree (no copies; caller-supplied JSON lives
     * for the duration of this handler). */
    int n = cJSON_GetArraySize(mountpoints_node);
    const char **mountpoints = calloc((size_t)n, sizeof(char *));
    if (!mountpoints) {
        *err_class = "GenericError";
        *err_desc = "Out of memory";
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(mountpoints_node, i);
        if (!cJSON_IsString(item) || !item->valuestring) {
            free(mountpoints);
            *err_class = "GenericError";
            *err_desc = "mountpoints entries must be strings";
            return NULL;
        }
        mountpoints[i] = item->valuestring;
    }

    /* Idempotent: if already frozen, return the current count. */
    if (freeze_status) {
        free(mountpoints);
        return cJSON_CreateNumber(frozen_volume_count);
    }

    LOG_INFO("Filesystem freeze starting (subset: %d mountpoint(s))", n);

    /* Step 1: Run freeze hooks. */
    if (run_hooks("freeze") < 0) {
        free(mountpoints);
        *err_class = "GenericError";
        *err_desc = "Freeze hook script failed";
        return NULL;
    }

    /* Step 2: APFS snapshot deliberately SKIPPED for subset freezes —
     * see function header comment. Per-mount F_FULLFSYNC carries the
     * consistency burden here. */
    fs_counts_t counts;
    memset(&counts, 0, sizeof(counts));

    /* Step 3: sync + per-FS dispatch, restricted to the listed mountpoints. */
    int total = sync_all_volumes(1, &counts, mountpoints, n);

    free(mountpoints);

    if (total == 0) {
        *err_class = "GenericError";
        *err_desc = "None of the listed mountpoints could be frozen";
        run_hooks("thaw");
        return NULL;
    }

    /* Step 4: Set frozen state. */
    freeze_status = 1;
    freeze_start_time = time(NULL);
    frozen_volume_count = total;
    auto_thaw_fired = 0;

    /* Step 5: Set auto-thaw alarm. */
    signal(SIGALRM, auto_thaw_handler);
    alarm(AUTO_THAW_SECS);

    int skipped = counts.skipped_network + counts.skipped_special +
                  counts.skipped_readonly;
    LOG_INFO("Filesystem frozen (subset, %d requested): %d snapshotted, "
             "%d zfs_snapshotted, %d fullfsynced, %d flushed_only (=%d total); "
             "skipped %d (%d network, %d special, %d readonly)",
             n,
             counts.snapshotted, counts.zfs_snapshotted,
             counts.fullfsynced, counts.flushed_only, total,
             skipped,
             counts.skipped_network, counts.skipped_special, counts.skipped_readonly);

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

    /* Clean up any ZFS snapshots taken during this freeze cycle */
    delete_zfs_snapshots();

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

/* ---- Freeze-safe command check ----
 *
 * Conservative-by-default per the design spec
 * (docs/design/AGENT_BEHAVIOUR_SPEC.md Q5): a command is allowed during
 * freeze iff (a) its handler does not write to any file or device,
 * (b) does not execute external programs, and (c) does not change the
 * agent's freeze state except by reporting status, by idempotent re-entry
 * into the current state, or by exiting via thaw.
 *
 * Current allowlist = upstream's six (ping/info/sync/sync-delimited/
 * fsfreeze-status/fsfreeze-thaw) plus our `guest-sync-id` extension and
 * idempotent re-freeze (`fsfreeze-freeze` / `fsfreeze-freeze-list`). The
 * idempotent re-freeze is a deliberate divergence from upstream — our
 * handler returns the current count without double-freeze damage, which
 * benefits backup tools that retry on timeout. Documented in the spec.
 *
 * Read-only inspection commands (guest-get-*, guest-network-get-*, file
 * read ops, etc.) are intentionally NOT on this list. Although our
 * pseudo-freeze isn't a true I/O suspension and reads would be
 * functionally safe, we have no concrete consumer demand to justify
 * the surface expansion. Expand only when a real use case appears —
 * see the spec doc for the rule.
 */

/* Pure-function variant — checks only the allowlist, ignores freeze_status.
 * Exposed (non-static) for unit testing in tests/test_proactive.c. */
int fsfreeze_is_allowlisted(const char *cmd_name)
{
    if (!cmd_name) return 0;
    static const char *allowed[] = {
        "guest-ping",
        "guest-sync",
        "guest-sync-id",                 /* our extension; harmless during freeze */
        "guest-sync-delimited",
        "guest-info",
        "guest-fsfreeze-status",
        "guest-fsfreeze-freeze",         /* idempotent — divergence from upstream */
        "guest-fsfreeze-freeze-list",    /* idempotent — divergence from upstream */
        "guest-fsfreeze-thaw",
        NULL
    };
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(cmd_name, allowed[i]) == 0) return 1;
    }
    return 0;
}

int fsfreeze_command_allowed(const char *cmd_name)
{
    if (!freeze_status) return 1;          /* Not frozen — everything is allowed. */
    return fsfreeze_is_allowlisted(cmd_name);
}

/* Public, side-effect-free check used by --self-test-json so operators can
 * confirm at install time whether ZFS dispatch will use `zfs snapshot` or
 * fall back to F_FULLFSYNC. find_zfs_cli() caches its result so repeated
 * calls are cheap. */
int fsfreeze_has_zfs_cli(void)
{
    return find_zfs_cli() != NULL;
}

/* ---- Init ---- */

void cmd_fs_init(void)
{
    command_register("guest-fsfreeze-status", handle_fsfreeze_status, 1);
    command_register("guest-fsfreeze-freeze", handle_fsfreeze_freeze, 1);
    command_register("guest-fsfreeze-freeze-list", handle_fsfreeze_freeze_list, 1);
    command_register("guest-fsfreeze-thaw", handle_fsfreeze_thaw, 1);
    command_register("guest-fstrim", handle_fstrim, 1);
}
