# Dead-item audit — QGA commands

> **Superseded (issue #12):** the `guest-fstrim` row below records the v2.5.5
> decision (turn the dead no-op into an honest error). That was itself reverted —
> `guest-fstrim` is now **not registered at all** on macOS (matches upstream
> `CONFIG_FSTRIM` gating; calling it returns `CommandNotFound`). Command count is
> now 44. See `docs/RECLAIM.md`.


Goal: find every command that returns success/data without actually doing the
thing — the anti-pattern that makes a caller think work happened when nothing
did (same class as the issue #11 empty-array lie). Audited by reading every
handler in `src/cmd-*.c`.

## Findings

### Dead (returned success, did nothing) — FIXED

| Command | Old behavior | Fix |
|---------|--------------|-----|
| `guest-fstrim` | always `{"paths":[]}`, no trim, every platform (`cmd-fs.c` "documented no-op") | **honest error**: "not supported on macOS; TRIM/UNMAP is automatic on delete (discard=on + ssd=1). No on-demand trim to invoke." |
| `guest-set-time` (argless) | `{}` success without acting (my earlier change) | **honest error**: "argless RTC resync not supported on macOS (no userspace hwclock); pass explicit 'time'." |

### Misleading (real-looking data, wrong meaning) — FIXED

| Command | Old behavior | Fix |
|---------|--------------|-----|
| `guest-get-memory-blocks` | `online` computed from memory *used* (`used/block_size`) — conflated "used" with "online", so a half-used VM looked half-unplugged | report the hotplug truth: every block `online:true, can-offline:false` (macOS RAM is never offline-able). Removed the now-dead `vm_stat`/`vm_stat_mach`/`vm_stat_text` helpers + `host_statistics64` weak-import (≈120 lines of dead code). |

### Honest already (NOT dead — correctly error/advertise)

- `guest-set-vcpus`, `guest-set-memory-blocks` — registered `enabled:0`; return "hotplug not supported on macOS." Correct.

### Platform-limited (could not cleanly succeed; wedged instead) — FIXED (v2.5.5)

| Command | Old behavior | Fix |
|---------|--------------|-----|
| `guest-suspend-ram` / `guest-suspend-hybrid` | real `pmset sleepnow` with an S3 hibernate mode; QEMU has no in-guest wake path, so it *wedged the VM* instead of failing — while still advertising `enabled:true` | **gated**: registered `enabled:0` so `guest-info` advertises `enabled:false`; the normal path returns `CommandNotFound`. If an operator force-enables via `--allow-rpcs`, the handler returns a `GenericError` pointing to host-side suspend (`qm suspend` / `virsh suspend`). Verified reachable via `--allow-rpcs` (so it is the force-enable fallback, not dead code). |

- `guest-suspend-disk` — unchanged: real `pmset` hibernatemode 25 (write image +
  power off, host-resumable). Stays `enabled:true`; it can cleanly succeed.

### The other 39 commands are real

ping/sync/sync-id/sync-delimited, info, get-osinfo, get-host-name/get-hostname,
get-timezone, get-time, get-users (real `who` fallback), get-load, get-vcpus,
get-cpustats, get-disks, get-diskstats, get-fsinfo, get-memory-block-info,
network-get-interfaces, network-get-route, file-open/read/write/seek/flush/close,
exec, exec-status, ssh-get/add/remove-authorized-keys, set-user-password,
set-time (explicit), fsfreeze-freeze/status/thaw/freeze-list, shutdown — all
verified doing real work in the command battery (53/53 on all 4 VMs).

## Verification

- Unit suite: 128 passed, 0 failed (assertions updated to require the honest
  errors).
- Live: SL 113 command battery 53/53 with the fixed binary; the QGA channel
  stayed alive after removing the inert B115200 baud setting.
- Pending: redeploy the fixed binary to Tiger/Leopard/BAM and re-run their
  batteries (SL proves the behavior; the others are mechanical re-runs).
