#ifndef MGA_CMD_FS_H
#define MGA_CMD_FS_H

/* Set test/dry-run mode — freeze simulates without touching real filesystems */
void fsfreeze_set_test_mode(int enabled);

/* Called from agent.c poll loop to check if frozen and run continuous sync */
int fsfreeze_is_frozen(void);
void fsfreeze_continuous_sync(void);

/* Called from agent.c to check if a command is allowed during freeze */
int fsfreeze_command_allowed(const char *cmd_name);

/* Pure-function variant — checks only the allowlist, ignores freeze_status.
 * Exposed for unit testing (see tests/test_proactive.c). Returns 1 if the
 * command is in the freeze-safe allowlist, 0 otherwise. */
int fsfreeze_is_allowlisted(const char *cmd_name);

/* ---- Per-filesystem-type dispatch (see docs/design/AGENT_BEHAVIOUR_SPEC.md Q1) ---- */

typedef enum {
    FS_SKIP_RDONLY,      /* MNT_RDONLY set */
    FS_SKIP_NETWORK,     /* smbfs, afpfs, nfs, webdav, ftp */
    FS_SKIP_SPECIAL,     /* devfs, fdesc, volfs, synthfs, lifs, autofs, or non-/dev backing */
    FS_APFS_WRITABLE,    /* APFS — tmutil snapshot at container level (separate path) + F_FULLFSYNC per mount */
    FS_ZFS_WRITABLE,     /* OpenZFS-on-macOS — zfs snapshot if CLI present (Phase 2 item 3); F_FULLFSYNC fallback */
    FS_HFS_WRITABLE,     /* HFS+ — F_FULLFSYNC (primary path) */
    FS_GENERIC_WRITABLE  /* unknown writable type backed by /dev/ — try F_FULLFSYNC, tolerate ENOTSUP */
} fs_class_t;

/* Per-treatment counts emitted by the freeze sync loop. */
typedef struct {
    int snapshotted;      /* APFS snapshot taken (0 or 1 — one per container) */
    int zfs_snapshotted;  /* ZFS snapshot taken (per dataset) */
    int fullfsynced;      /* F_FULLFSYNC succeeded on this mount */
    int flushed_only;     /* F_FULLFSYNC unavailable or failed; the global sync() covered the data */
    int skipped_network;
    int skipped_special;
    int skipped_readonly;
} fs_counts_t;

/* Classify a mount for freeze dispatch. Pure function — no I/O.
 * Exposed (non-static) for unit testing in tests/test_proactive.c. */
struct statfs;
fs_class_t fs_dispatch_class(const struct statfs *mnt);

#endif
