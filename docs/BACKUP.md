# Backup & Filesystem Freeze

The agent implements real filesystem freeze for consistent PVE backups — not a simulation.

## How Freeze Works

**On freeze (`guest-fsfreeze-freeze`):**
1. Runs hook scripts from `/etc/qemu/fsfreeze-hook.d/` (for database flush, service pause, etc.)
2. APFS (10.13+): creates an atomic COW snapshot via `tmutil localsnapshot` — this is the consistency point
3. All versions: `sync()` + `F_FULLFSYNC` flushes all data to physical media
4. Continuous `sync()` every 100ms during the freeze window to catch new writes
5. Auto-thaw after 10 minutes if PVE never sends thaw (safety net)
6. Commands are restricted during freeze (only ping, sync, info, freeze/thaw allowed)

**On thaw (`guest-fsfreeze-thaw`):**
1. Cleans up APFS snapshot
2. Runs thaw hooks in reverse order
3. Restores normal operation

## Freeze Methods by macOS Version

| macOS | Method | Consistency Level |
|---|---|---|
| 10.4–10.12 | `sync()` + `F_FULLFSYNC` | Disk-level flush (equivalent to most Linux VMs without LVM) |
| 10.13+ | `sync()` + `F_FULLFSYNC` + APFS snapshot | Point-in-time consistent (best available on macOS) |

macOS has no kernel-level filesystem freeze (FIFREEZE) like Linux. VMware Tools for Mac never supported quiesced snapshots either. Our implementation provides the best consistency guarantee available on macOS.

**Note on `guest-fsfreeze-freeze-list`:** This command accepts a mountpoint list parameter but currently freezes all filesystems regardless — the mountpoint filter is not yet implemented. In practice this rarely matters because macOS VMs typically have a single data volume, so freezing everything is the correct behavior. If you need selective freeze, use hook scripts to manage specific services instead.

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
