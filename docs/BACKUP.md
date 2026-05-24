# Backup & Filesystem Freeze

The agent implements real filesystem freeze for consistent PVE backups — not a simulation.

## How Freeze Works

**On freeze (`guest-fsfreeze-freeze`):**
1. Runs hook scripts from `/etc/qemu/fsfreeze-hook.d/` (for database flush, service pause, etc.)
2. Global `sync(2)` flushes dirty buffers system-wide.
3. Per-mount dispatch by `f_fstypename` — APFS gets `tmutil localsnapshot` plus `F_FULLFSYNC`, HFS+ gets `F_FULLFSYNC`, FAT/exFAT/UDF/NTFS try `F_FULLFSYNC` and tolerate `ENOTSUP`, ZFS gets `zfs snapshot` when the CLI is present, network mounts (smbfs/afpfs/nfs/webdav/ftp) and special FS (devfs/autofs/fdesc/synthfs/volfs/lifs) are skipped categorically. The full table lives in [docs/design/FREEZE_SEMANTICS.md](design/FREEZE_SEMANTICS.md).
4. Continuous `sync()` every 100ms during the freeze window to catch new writes (best-effort; macOS has no `FIFREEZE`).
5. Auto-thaw after 10 minutes if PVE never sends thaw (safety net).
6. Commands are restricted during freeze. The allowlist is 9 commands: the upstream 6 (`guest-ping`, `guest-sync`, `guest-sync-delimited`, `guest-info`, `guest-fsfreeze-status`, `guest-fsfreeze-thaw`) plus our `guest-sync-id` extension and the two freeze handlers themselves (`guest-fsfreeze-freeze`, `guest-fsfreeze-freeze-list`) — see [FREEZE_SEMANTICS.md → Divergences from upstream QGA](design/FREEZE_SEMANTICS.md#divergences-from-upstream-qga).
7. Emits a single INFO log line summarising the per-treatment breakdown (snapshotted / zfs_snapshotted / fullfsynced / flushed_only / skipped) — the wire response is the spec-conformant `int` total; the breakdown is in the log.

**On thaw (`guest-fsfreeze-thaw`):**
1. Cleans up APFS snapshot (`tmutil deletelocalsnapshots` for the snapshot name tracked at freeze time).
2. Cleans up ZFS snapshots (`zfs destroy` for each snapshot tracked at freeze time).
3. Runs thaw hooks in reverse order.
4. Restores normal operation.

## What "freeze" means per filesystem

macOS has no kernel-level filesystem freeze (`FIFREEZE`) like Linux, and no `UFSSUSPEND` like FreeBSD. VMware Tools for Mac never supported quiesced snapshots either. What the agent does is the best consistency guarantee available on macOS, applied per filesystem type:

| `f_fstypename` | Treatment | Guarantee |
|---|---|---|
| `apfs` | `tmutil` snapshot + `F_FULLFSYNC` per mount | Atomic point-in-time view (10.13+) |
| `hfs` | `F_FULLFSYNC` per mount | Disk-level flush (10.4–10.12; equivalent to a Linux ext4 backup without LVM) |
| `zfs` (OpenZFS-on-macOS, CLI present) | `zfs snapshot` per dataset | Atomic point-in-time view |
| `msdos` / `vfat` / `exfat` / `udf` / `ntfs` | `F_FULLFSYNC`, tolerate `ENOTSUP` | Disk-level flush (covered by global `sync` if the fcntl is unimplemented) |
| `smbfs` / `afpfs` / `nfs` / `webdav` / `ftp` | Skipped | Remote mount — backing server owns its own consistency |
| `devfs` / `autofs` / `fdesc` / `synthfs` / `volfs` / `lifs` | Skipped | Synthetic/pseudo FS, no backing storage |

The full dispatch table — including the `_default_writable_dev_backed` and `_default_unknown_non_dev` sentinels, the failure-mode classification, and the divergences from upstream QGA — is in [docs/design/FREEZE_SEMANTICS.md](design/FREEZE_SEMANTICS.md). The same table is surfaced statically by `mac-guest-agent --self-test-json` under `freeze_dispatch.per_fstypename`, so you can introspect the policy without running a freeze.

**`guest-fsfreeze-freeze-list`** accepts a `mountpoints` JSON array argument and freezes only those mounts (since v2.4.3 — earlier versions silently ignored the argument and froze globally). With no argument or an empty array it delegates to the global `guest-fsfreeze-freeze` handler. Subset freezes deliberately skip the container-level APFS snapshot even when an APFS mount is in the list — container-level snapshots are not partitionable per-mount, so taking one for a subset request would snapshot state the caller didn't ask us to capture. APFS mounts in a subset freeze get the same per-mount `F_FULLFSYNC` treatment as HFS+ mounts.

Verified on El Capitan 10.11.6 with LVM snapshot + mount test (290/290 stress cycles clean).

## Hook Scripts

Drop-in scripts for `/etc/qemu/fsfreeze-hook.d/` that run during freeze and thaw. Ready-made hooks are in `configs/hooks/`:

| Script | Application | On Freeze | On Thaw |
|---|---|---|---|
| `mysql.sh` | MySQL / MariaDB | FLUSH TABLES WITH READ LOCK | Release lock |
| `postgresql.sh` | PostgreSQL | CHECKPOINT | Nothing (auto-resumes) |
| `redis.sh` | Redis | BGSAVE + wait | Nothing (auto-resumes) |
| `launchd-service.sh` | Any launchd service | Stop services | Restart services |

### Install a hook:
```bash
sudo cp configs/hooks/mysql.sh /etc/qemu/fsfreeze-hook.d/
sudo chmod 755 /etc/qemu/fsfreeze-hook.d/mysql.sh
sudo chown root:wheel /etc/qemu/fsfreeze-hook.d/mysql.sh
```

### Requirements:
- Must be owned by root (uid 0)
- Must not be world-writable
- Must be executable (`chmod 755`)
- 30-second timeout per script
- Scripts run alphabetically on freeze, reverse on thaw
- Prefix with numbers to control order: `00-mysql.sh`, `10-redis.sh`

### Writing a custom hook:
```bash
#!/bin/bash
case "$1" in
    freeze)
        # Called BEFORE filesystem freeze
        # Flush buffers, acquire locks, pause writes
        ;;
    thaw)
        # Called AFTER filesystem thaw
        # Release locks, resume writes
        ;;
esac
exit 0
```

A non-zero exit code logs a warning but does NOT abort the freeze.

## Backup Readiness Check

```bash
sudo mac-guest-agent --self-test
```

The self-test reports:
- **Freeze method:** APFS snapshot / sync+F_FULLFSYNC / sync only
- **Freeze capability:** root required for real freeze
- **Hook validation:** count, ownership, permissions
- **Overall verdict:** ready / needs attention

JSON output for automation:
```bash
sudo mac-guest-agent --self-test-json
```

## Thin Disk Provisioning (TRIM)

Reclaim free space from thin-provisioned virtual disks.

### PVE Host:
```bash
qm set <vmid> --sata0 <storage>:vm-<vmid>-disk-1,discard=on,ssd=1
```
Requires VM stop/start (not just reboot).

### macOS VM (one-time):
```bash
sudo trimforce enable    # Requires reboot
```

### Verify:
```bash
diskutil info disk0 | grep -i "Solid State\|TRIM"
# Should show: Solid State: Yes, TRIM Support: Yes
```

After this, macOS sends TRIM on every file delete. Space is reclaimed on the host in real-time. The `guest-fstrim` command is a no-op because macOS handles TRIM natively.

### Reclaim existing free space (one-time):
```bash
dd if=/dev/zero of=/tmp/.reclaim bs=4m 2>/dev/null; rm -f /tmp/.reclaim; sync
```
QEMU's `detect-zeroes=unmap` reclaims the space on the host.
