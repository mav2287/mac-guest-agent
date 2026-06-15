# Command Status

All 42 registered commands with their actual status, Linux parity, and requirements. Some commands are restricted during filesystem freeze — see [SECURITY.md](../SECURITY.md#freeze-state-command-restrictions) for details.

> **Intentionally not registered on macOS** (no equivalent API; matches upstream QEMU's `CONFIG_*` gating — we omit the command rather than ship a stub, so `guest-info` doesn't advertise it and calling it returns the standard `CommandNotFound`):
> - `guest-fstrim` — on-demand bulk free-space discard (Linux `FITRIM`); macOS has no volume-level free-extent trim API. To reclaim thin-provisioned space see [RECLAIM.md](RECLAIM.md).
> - `guest-set-vcpus` — no CPU hotplug on macOS.
> - `guest-set-memory-blocks` — no memory hotplug on macOS.

## Status Key

| Status | Meaning |
|---|---|
| **stable** | Works as expected, tested |
| **caveated** | Works with documented limitations |
| **no-op** | Returns success but does nothing (by design) |
| **error** | Returns error (macOS doesn't support this operation) |
| **alias** | Duplicate registration of another command |

## Linux Parity Key

| Parity | Meaning |
|---|---|
| **full** | Same behavior as Linux qemu-ga |
| **partial** | Same function, different mechanism or reduced detail |
| **divergent** | Fundamentally different or unsupported on macOS |

## Command Table

| Command | Category | Status | Parity | Root | Notes |
|---|---|---|---|---|---|
| `guest-ping` | protocol | stable | full | no | |
| `guest-sync` | protocol | stable | full | no | |
| `guest-sync-id` | protocol | alias | full | no | Alias for guest-sync |
| `guest-sync-delimited` | protocol | stable | full | no | 0xFF delimiter framing |
| `guest-info` | protocol | stable | full | no | |
| `guest-get-osinfo` | system | stable | full | no | "Mac OS X" on <10.12, "macOS" on 10.12+ |
| `guest-get-host-name` | system | stable | full | no | |
| `guest-get-hostname` | system | alias | full | no | Alias for guest-get-host-name |
| `guest-get-timezone` | system | stable | full | no | |
| `guest-get-time` | system | stable | full | no | gettimeofday (not clock_gettime) |
| `guest-set-time` | system | stable | full | yes | settimeofday + date fallback |
| `guest-get-users` | system | stable | full | no | Via getutxent |
| `guest-get-load` | system | stable | full | no | Via getloadavg |
| `guest-shutdown` | power | stable | full | yes | osascript → shutdown fallback |
| `guest-suspend-disk` | power | stable | partial | yes | Via pmset (not /sys/power/state) |
| `guest-suspend-ram` | power | stable | partial | yes | Via pmset |
| `guest-suspend-hybrid` | power | stable | partial | yes | Via pmset |
| `guest-get-vcpus` | hardware | stable | partial | no | All reported online, can-offline=false |
| `guest-get-memory-blocks` | hardware | stable | partial | no | Synthetic blocks derived from real memory usage |
| `guest-get-memory-block-info` | hardware | stable | partial | no | Block size derived from total memory |
| `guest-get-cpustats` | hardware | stable | partial | no | Via Mach host_statistics (not /proc/stat) |
| `guest-get-disks` | disk | stable | partial | no | Via diskutil, no PCI address mapping |
| `guest-get-fsinfo` | disk | stable | full | no | Via getmntinfo + statfs |
| `guest-get-diskstats` | disk | stable | partial | no | IOKit `IOBlockStorageDriver` Statistics → 6 spec fields (read/write sectors/ios/ticks); 9 Linux-only fields (merges, discard*, in-flight, io-ticks, time-in-queue) zero-valued; major/minor=0 (no macOS equivalent) |
| `guest-fsfreeze-freeze` | fs | caveated | partial | yes | sync + F_FULLFSYNC + continuous sync. APFS snapshot on 10.13+. Not kernel-level FIFREEZE. |
| `guest-fsfreeze-freeze-list` | fs | caveated | partial | yes | Subset freeze: filters by supplied `mountpoints` array (freezes only listed paths). Empty/absent list behaves like `guest-fsfreeze-freeze` (all). Subset freezes skip the container-level APFS snapshot. See `docs/BACKUP.md` and `src/cmd-fs.c handle_fsfreeze_freeze_list()`. |
| `guest-fsfreeze-thaw` | fs | caveated | partial | yes | Cleans up APFS snapshot, runs thaw hooks |
| `guest-fsfreeze-status` | fs | stable | full | no | Reflects actual freeze state |
| `guest-network-get-interfaces` | network | stable | full | no | Via getifaddrs, AF_LINK for MAC addresses |
| `guest-network-get-route` | network | stable | full | no | Via netstat -rn, IPv4 + IPv6 routes |
| `guest-file-open` | file | stable | full | yes | Handle table, max 64 open files |
| `guest-file-close` | file | stable | full | no | |
| `guest-file-read` | file | stable | full | no | Max 48KB per read, base64 encoded |
| `guest-file-write` | file | stable | full | no | Base64 decoded input |
| `guest-file-seek` | file | stable | full | no | |
| `guest-file-flush` | file | stable | full | no | Via fsync |
| `guest-exec` | exec | stable | full | yes | fork + execvp with pipe capture |
| `guest-exec-status` | exec | stable | full | no | waitpid with raw status for signal detection |
| `guest-ssh-get-authorized-keys` | ssh | stable | full | yes | Via getpwnam + file read |
| `guest-ssh-add-authorized-keys` | ssh | stable | full | yes | Dedup, creates .ssh dir if needed |
| `guest-ssh-remove-authorized-keys` | ssh | stable | full | yes | Filter and rewrite |
| `guest-set-user-password` | user | caveated | partial | yes | Via dscl stdin pipe (not chpasswd) |

## Summary

- **Stable:** 36 commands
- **Caveated:** 4 commands (fsfreeze-freeze, fsfreeze-freeze-list, fsfreeze-thaw, set-user-password)
- **Alias:** 2 commands (sync-id, get-hostname)
- (Total registered: 42)
- **Full Linux parity:** 29 commands
- **Partial parity:** 13 commands
- **Not registered** (no macOS equivalent, matches upstream): `guest-fstrim`, `guest-set-vcpus`, `guest-set-memory-blocks` — see note at top + [RECLAIM.md](RECLAIM.md)

## Runtime Test Evidence

| Command | El Capitan 10.11.6 (PVE) | Tahoe 26.3 (test mode) |
|---|---|---|
| All protocol commands | Passed | Passed |
| All system commands | Passed | Passed |
| Power commands | Skipped (destructive) | Skipped |
| All hardware commands | Passed | Passed |
| All disk/fs commands | Passed (freeze verified with LVM snapshot + mount) | Passed (dry-run) |
| All network commands | Passed | Passed |
| All file commands | Passed | Passed |
| All exec commands | Passed | Passed |
| SSH commands (error path) | Passed | Passed |
| SSH commands (success path) | Not tested on PVE | Passed |
| Password command | Not tested | Not tested |
