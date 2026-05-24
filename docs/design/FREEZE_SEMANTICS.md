# Freeze Semantics on macOS

This document defines, precisely, what `guest-fsfreeze-freeze`,
`guest-fsfreeze-freeze-list`, and `guest-fsfreeze-thaw` actually do when
the guest is macOS — per filesystem type, what the operation guarantees,
where the implementation deliberately diverges from the upstream QEMU
Guest Agent (QGA), and what the failure modes are.

It is the single source of truth that contributors, operators, and the
verifier tooling (`scripts/verify.sh`, `--self-test-json`) cross-reference.
The dispatch table here mirrors `fs_dispatch_class()` in `src/cmd-fs.c` and
the `freeze_dispatch` block emitted by `--self-test-json`. If the three
ever disagree, the **code is canonical** and the docs are stale.

---

## macOS has no `FIFREEZE`

Linux exposes `FIFREEZE`/`FITHAW` ioctls that genuinely **suspend** a
filesystem at the VFS layer: every subsequent write blocks in the kernel
until thaw. FreeBSD provides the analogous `UFSSUSPEND`/`UFSRESUME` for
UFS. Both are true I/O quiescence.

macOS provides **neither**. There is no public kernel interface on any
version of macOS — 10.4 through 26.x — that suspends writes to an APFS,
HFS+, or any other locally-mounted filesystem. VMware Tools for Mac
never shipped quiesced snapshots either, for the same reason.

What macOS *does* provide, and what this agent uses, is:

- `tmutil localsnapshot` (10.13+): triggers an APFS container-level
  copy-on-write snapshot. This is an *atomic consistency point* —
  everything committed to the container at the moment of snapshot is
  preserved as a coherent point-in-time view — but it does **not**
  suspend subsequent writes. Reads and writes continue normally; they
  just don't appear in the snapshot.
- `F_FULLFSYNC` (all versions, on filesystems that implement it):
  Apple's documented `fcntl(2)` that flushes the file's data **and the
  filesystem's pending writes for that volume** all the way to the
  storage device — past the OS buffer cache and past the drive's own
  write cache. Stronger than `fsync(2)`, which only flushes to the
  device buffer. Per Apple's `fcntl(2)` man page, `F_FULLFSYNC` is
  implemented on **HFS, MS-DOS (FAT), UDF, and APFS**.
- `sync(2)`: best-effort flush of all dirty buffers system-wide. Cheap,
  no per-volume guarantees, but provides cover for filesystems where
  `F_FULLFSYNC` is unimplemented (FUSE, exFAT on older drivers, etc.).
- `zfs snapshot` (OpenZFS-on-macOS, third-party): atomic ZFS snapshot
  at the dataset level. The agent uses this when the `zfs` CLI is
  detected at one of the standard install paths.

The freeze implementation combines these primitives into a per-`f_fstypename`
dispatch table.

---

## Dispatch table (per `f_fstypename`)

The values returned by `getfsstat(2)` in `struct statfs::f_fstypename`
are the dispatch key. The table below is the policy applied by
`sync_all_volumes()` after the global `sync(2)`. The same table is
surfaced statically in `--self-test-json` under
`freeze_dispatch.per_fstypename` so anyone can introspect the policy
without running a freeze.

| `f_fstypename` | Treatment | Consistency primitive |
|---|---|---|
| `apfs` | `tmutil_snapshot+f_fullfsync` | APFS container-level COW snapshot via `tmutil localsnapshot` (the consistency point) **plus** per-mount `F_FULLFSYNC` (cleans dirty buffers between snapshot and read-out). One snapshot per container, regardless of how many APFS volumes are mounted off it. Snapshot name tracked so `do_thaw` can delete it via `tmutil deletelocalsnapshots`. |
| `hfs` | `f_fullfsync` | Per-mount `F_FULLFSYNC`. HFS+ has no snapshot mechanism, so this is a *flush*, not an atomic point-in-time view. PVE backups of HFS+ volumes are consistent at the disk-flush level (the same level a Linux ext4 backup without LVM snapshots gets). |
| `msdos`, `vfat`, `exfat`, `udf`, `ntfs` | `f_fullfsync_with_enotsup_tolerated` | Try `F_FULLFSYNC`. If the kernel returns `ENOTSUP`/`EOPNOTSUPP` (FUSE-backed mounts, third-party drivers, older revisions of the bundled drivers), log `DEBUG` only and count the mount as `flushed_only` — the global `sync(2)` at the top of the handler already covered the data. Apple's `fcntl(2)` man page lists MS-DOS and UDF as supported, so on the in-tree drivers this path should succeed. Matches upstream QGA's behaviour for Linux mounts that don't implement `FIFREEZE`. |
| `zfs` | `zfs_snapshot_if_cli_else_f_fullfsync` | If the `zfs` CLI is present at `/usr/local/sbin/zfs`, `/usr/local/bin/zfs`, `/opt/local/bin/zfs`, or `/opt/homebrew/bin/zfs` (OpenZFS-on-macOS install paths), take an atomic `zfs snapshot <pool>/<dataset>@mac-guest-agent-<timestamp>`. Snapshot names tracked so `do_thaw` can `zfs destroy` them. Fall through to per-mount `F_FULLFSYNC` if the CLI is absent — `F_FULLFSYNC` on ZFS is not Apple-documented but is defence in depth. |
| `smbfs`, `afpfs`, `nfs`, `webdav`, `ftp` | `skip_network` | Counted as `skipped_network`. Freezing a remote mount is not the local guest's responsibility — the backing server owns its own consistency. Categorically skipped. |
| `devfs`, `autofs`, `fdesc`, `volfs`, `synthfs`, `lifs` | `skip_special` | Counted as `skipped_special`. Synthetic / pseudo / on-demand mounts. No backing storage to flush. |
| read-only mount (any type, `MNT_RDONLY` set) | `skip_readonly` | Counted as `skipped_readonly`. No dirty buffers to flush. |
| unknown writable type backed by `/dev/...` | `f_fullfsync_with_enotsup_tolerated` (`_default_writable_dev_backed`) | Same treatment as the FAT/exFAT/UDF/NTFS row. `ENOTSUP` tolerated. |
| unknown type with non-`/dev/...` backing | `skip_special` (`_default_unknown_non_dev`) | Defence-in-depth — anything we can't classify and that isn't on real storage is skipped to avoid mishandling. |

The wire response of `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list`
remains a single integer (`int`), matching the QGA spec
(`['guest-fsfreeze-freeze']` returns `int`). The per-treatment breakdown
is surfaced only in (a) the agent log INFO line, (b) the
`--self-test-json` `freeze_dispatch` block, and (c) the verifier output
of `scripts/verify.sh` which fetches the log line via
`qm agent <vmid> exec`. Extending the wire response with a structured
breakdown would violate the spec for the only field PVE actually reads.

---

## `freeze-list` (subset freeze)

`guest-fsfreeze-freeze-list` accepts an optional `mountpoints` argument:
a JSON array of mount-point strings (e.g. `["/Volumes/data"]`). The
handler:

- with **no argument or an empty array** — delegates to the global
  `guest-fsfreeze-freeze` handler. This is the spec's default behaviour.
- with **one or more mount points** — restricts the per-FS dispatch loop
  to mounts whose `f_mntonname` matches one of the listed paths. Other
  mounts are unaffected (not flushed, not counted).

Subset freezes **deliberately skip** the container-level
`tmutil localsnapshot` even when an APFS mount is in the list. The
container-level snapshot is not partitionable per-mount — it captures
the whole container — so taking it for a subset request would snapshot
state the caller didn't ask us to capture. Per-mount `F_FULLFSYNC` is
the consistency mechanism for subset freezes; APFS mounts in a subset
get the same flush treatment as HFS+ mounts get in a global freeze.

If a caller needs an APFS-snapshot-consistent subset, they should call
the global `guest-fsfreeze-freeze` (which snapshots the container) and
treat the other mounts as collateral.

---

## What "frozen" status guarantees

After `guest-fsfreeze-freeze` (or `guest-fsfreeze-freeze-list`) returns
a non-error response, `guest-fsfreeze-status` reports `frozen` and the
agent has applied the dispatch table above. The guarantees are:

- For each **`apfs` mount in a global freeze**: every write that was
  committed to the container before the snapshot point exists in the
  snapshot. The backing image taken now will see the snapshot's
  consistent point-in-time view, even though new writes continue to
  hit the live container.
- For each **`hfs`, `msdos`, `vfat`, `exfat`, `udf`, `ntfs` mount that
  returned `fullfsynced`**: every write `close()`d on that volume before
  freeze is now on physical media (past the OS cache and past the
  drive's write cache).
- For each mount counted as **`flushed_only`**: the global `sync(2)` at
  the top of the handler flushed dirty buffers to the device; the
  device's own write cache may still hold them.
- For each **`zfs` mount in a global freeze with `zfs_cli_available`**:
  an atomic ZFS snapshot exists at the dataset level — equivalent to
  the APFS guarantee but per-dataset, not per-container.

The guarantees the agent does **not** provide, and that the upstream
Linux QGA *does* provide via `FIFREEZE`:

- **No I/O suspension.** Reads and writes continue throughout the freeze
  window. A write that happens during the freeze is not guaranteed to be
  in any backup taken from outside the VM. The atomic snapshot
  (APFS/ZFS) gives you a consistent *prior* state regardless; the
  `F_FULLFSYNC` flush gives you a guarantee about *prior* writes but
  not subsequent ones.
- **No application quiesce.** Hook scripts (`/etc/qemu/fsfreeze-hook.d/`)
  run before and after the freeze for app-level coordination, but the
  agent has no kernel-level interlock — applications can still issue
  writes during the freeze window.

These are macOS-platform limits, not implementation gaps. There is no
public macOS API that would provide I/O suspension.

A continuous `sync(2)` loop runs every 100 ms during the freeze window
to catch writes that happen between freeze and thaw. This is best-effort
defence in depth; it is not equivalent to `FIFREEZE`.

After 10 minutes with no thaw, the agent auto-thaws to prevent a
stuck-frozen state if PVE crashes mid-backup.

---

## Divergences from upstream QGA

These are deliberate, documented divergences from
`qemu/qga/commands-posix-ssh.c` and the Linux freeze path in
`qemu/qga/commands-posix.c`. They are also surfaced in
`--self-test-json` under `freeze_dispatch.divergences_from_upstream_qga`.

| Divergence | Upstream behaviour | Our behaviour | Reason |
|---|---|---|---|
| **Idempotent re-freeze** | Calling `guest-fsfreeze-freeze` while already frozen returns `GenericError: command 'guest-fsfreeze-freeze' failed: already frozen` (see upstream `qmp_guest_fsfreeze_freeze` `if (ga_is_frozen(ga_state)) { ... }`). | Returns the same count it returned for the original freeze; logs a debug line; no state mutation. | macOS has no `FIFREEZE`, so a duplicate "freeze" is a no-op in terms of kernel state. PVE occasionally retries the freeze command when its first response is delayed; failing the retry would convert benign retries into spurious backup failures. The risk of the upstream behaviour (a stuck-frozen state if the agent restarts mid-freeze) doesn't apply here because we have no kernel-level suspension to leak. |
| **No persistent frozen-state marker** | Upstream marks the agent process as frozen so that a daemon restart while frozen surfaces an error. | We track frozen state in-process only; a restart clears it. | Same reason — there is no kernel-level state to be inconsistent with. The 10-minute auto-thaw is the safety net. |
| **Logging during freeze** | Upstream disables logging while frozen to avoid log writes hitting a frozen filesystem and deadlocking. | We continue logging normally. | Same reason — our log file (`/var/log/mac-guest-agent.log`) is not on a frozen filesystem in the upstream sense, because no filesystem is suspended. Disabling logging would lose the per-event INFO summary that `scripts/verify.sh` greps for. |
| **`guest-sync-id` extension** | Not in upstream schema. | Allowed during freeze (it's read-only — returns a caller-supplied id verbatim). | Convenience for callers that want a per-request correlation token across a freeze/thaw cycle. Read-only, no state change, no I/O. |
| **Foreign-FS `F_FULLFSYNC` failure** | Upstream `FIFREEZE` returning `EOPNOTSUPP` is skipped silently (`qemu/qga/commands-posix.c` freeze loop). | `F_FULLFSYNC` returning `ENOTSUP`/`EOPNOTSUPP` is logged at `DEBUG` and the mount is counted as `flushed_only` (not as a failure). Other `errno` values (`EIO`, etc.) are logged at `WARN` and still counted as `flushed_only` rather than failing the whole freeze. | Mirrors the upstream "by-design, not a failure" treatment for the analogous case, applied to `F_FULLFSYNC` rather than `FIFREEZE`. Foreign filesystems where `F_FULLFSYNC` isn't implemented are still covered by the global `sync(2)`; failing the whole freeze for one foreign mount would convert a "you got a less-strong consistency guarantee for this one mount" condition into "your backup didn't run". |
| **Freeze-time command allowlist** | Upstream allowlist is a shared 6-command list (`guest-ping`, `guest-sync`, `guest-sync-delimited`, `guest-info`, `guest-fsfreeze-status`, `guest-fsfreeze-thaw`). | Our list is 9: the upstream 6 **plus** `guest-sync-id` (our extension), **plus** `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` (re-callable because of idempotent re-freeze above). | Each addition is justified by another documented divergence on this same table. The list is otherwise principled-restrictive: read-only / state-neutral commands only, no `exec`, no writes, no power/network/SSH/user mutations. |

---

## Failure modes

### Must-surface (we treat as freeze failure)

- **APFS `tmutil localsnapshot` fails on a global freeze on a 10.13+
  guest where `compat_has_apfs() && compat_has_tmutil()` is true.** This
  is the only consistency point we have on APFS; losing it means a
  global freeze provides no better guarantee than a flush. The freeze
  count is decremented for the container and a `WARN` is logged. The
  wire response is the count of mounts that *did* get the snapshot/flush,
  which may legitimately be zero if only an APFS container was in scope.
- **`zfs snapshot` fails when the `zfs` CLI is present.** ZFS is the
  consistency point for ZFS datasets; CLI present-but-failing is a real
  problem (pool issue, dataset busy). Logged `WARN` and counted as
  `flushed_only`.

### By-design (we treat as success)

- **`F_FULLFSYNC` returns `ENOTSUP`/`EOPNOTSUPP`** on a foreign
  filesystem. Counted as `flushed_only`, `DEBUG`-logged, freeze
  continues. Apple's `fcntl(2)` man page documents this as expected on
  filesystems that don't implement the fcntl; the global `sync(2)`
  covered the data.
- **`zfs` CLI absent.** ZFS dispatch falls through to `F_FULLFSYNC` (not
  Apple-documented for ZFS but defence in depth) and continues. No
  `WARN`; this is a host-configuration property the operator chose.
- **Network mounts present.** Categorically skipped. No `WARN`.
- **Special / pseudo-FS present** (devfs/autofs/etc.). Categorically
  skipped. No `WARN`.

### Surfaced for diagnosis (no behaviour change)

Every freeze emits a single INFO log line of the form:

```
Filesystem frozen: <S> snapshotted, <Z> zfs_snapshotted, <F> fullfsynced, <FO> flushed_only (=<T> total); skipped <K> (<N> network, <P> special, <R> readonly)
```

This is the per-event breakdown the wire response intentionally omits
(spec conformance). `scripts/verify.sh` will fetch it via
`qm agent <vmid> exec` (Phase 3) so the operator gets the breakdown
without needing to SSH into the guest.

---

## Cross-references

- **Code:** `src/cmd-fs.c` — `fs_dispatch_class()`, `sync_all_volumes()`,
  `handle_fsfreeze_freeze`, `handle_fsfreeze_freeze_list`,
  `create_zfs_snapshot()`, `delete_zfs_snapshots()`,
  `fsfreeze_is_allowlisted()`.
- **Spec doc:** `docs/design/AGENT_BEHAVIOUR_SPEC.md` — the design
  questions (Q1, Q2, Q3, Q5, Q6) that produced this dispatch table and
  these divergences.
- **Research:** `docs/research/UPSTREAM_NOTES.md` — primary-source
  evidence on the QGA spec, the Linux QGA reference implementation, the
  Proxmox QGA wrapper, the Windows VSS path, and Apple's
  `AppleQEMUGuestAgent`.
- **Verifier:** `scripts/verify.sh` — the host-side
  behavioural-check workflow.
- **Static introspection:** `mac-guest-agent --self-test-json`
  `freeze_dispatch` block — what the binary statically advertises about
  its dispatch policy.
- **Tests:** `tests/test_proactive.c` — 21-case `fs_dispatch_class`
  unit test, 30-case allowlist test; `tests/run_tests.sh` —
  `freeze_dispatch` JSON contract test, `freeze-list` integration tests.
