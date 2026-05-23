# Agent behaviour specification

**Status:** in progress. Phase 2 of `../PLAN.md`.

This document is the answer to the seven design questions Phase 1 surfaced (see `../research/UPSTREAM_NOTES.md`). Each section follows the same shape:

- **The question** (restated from PLAN.md).
- **The evidence** — what Phase 1 turned up plus any supplementary verification.
- **Options considered.**
- **Decision** — the design we'll implement against.
- **Implementation implications** — which source file changes, what log/response shape changes.
- **Failure modes** — what must surface to the user, what is by-design and must not look alarming.

Implementation work flows from this spec as separate commits. Nothing here changes code on its own.

---

## Question 1 — Per-filesystem-type freeze strategy

### The question

macOS has no `FIFREEZE`. We get exactly what the platform provides per filesystem type. What strategy do we run for each `statfs.f_fstypename` value, and how do we account for the differences honestly in the response and the log?

### Evidence

**Phase 1 (Targets 1, 2, 6):**

- Linux QGA's reference (`commands-linux.c`) treats `EOPNOTSUPP`/`EBUSY` from `FIFREEZE` as "skip silently, don't count, continue," and pre-filters network/non-device-backed mounts before the loop.
- Apple's own `AppleQEMUGuestAgent` implements **no freeze command at all** — backup consistency on a macOS guest is impossible with Apple's agent alone. We are filling a real gap, not duplicating.
- QGA spec defines `guest-fsfreeze-freeze` as returning `int` (count of frozen filesystems). The wire format is fixed; structured per-FS detail can only live in extensions or in log/JSON outputs.

**Supplementary verification on host macOS 26.5 (2026-05-23):**

Actual `f_fstypename` strings observed via `mount`:

```
apfs        — APFS (the modern Apple FS, snapshot-capable)
autofs      — automount placeholders
devfs       — /dev (special kernel FS)
smbfs       — SMB/CIFS network mount
```

(Other types — `hfs`, `msdos`/`vfat`, `exfat`, `cd9660`, `udf`, `ntfs`, `nfs`, `afpfs`, `webdav`, `fdesc`, `volfs`, `synthfs`, `lifs`, `zfs` — are common on real-world systems even if not present on this dev host.)

**Apple `fcntl(2)` man page on `F_FULLFSYNC`:**

> "Does the same thing as fsync(2) then asks the drive to flush all buffered data to the permanent storage device (arg is ignored). As this drains the entire queue of the device and acts as a barrier, data that had been fsync'd on the same device before is guaranteed to be persisted when this call returns. **This is currently implemented on HFS, MS-DOS (FAT), Universal Disk Format (UDF) and APFS file systems.** The operation may take quite a while to complete. Certain FireWire drives have also been known to ignore the request to flush their buffered data."

This is important: Apple documents `F_FULLFSYNC` support on `msdos`/`vfat`/`udf` — but vit9696's Tiger log shows it returning `ENOTSUP` on FAT32. The right reading is that Apple's *current* MS-DOS driver supports `F_FULLFSYNC`, and older drivers (Tiger-era) don't. Our handling must be version-agnostic: try `F_FULLFSYNC`, treat `ENOTSUP`/`EOPNOTSUPP` as expected on the FS+version combinations where the driver doesn't implement it.

**Time Machine local snapshots show as mounts:**

```
com.apple.TimeMachine.2026-05-21-044501.backup@/dev/disk9s1
    on /Volumes/.timemachine/.../...backup
    (apfs, local, nodev, nosuid, read-only, journaled, nobrowse)
```

These have `f_fstypename = apfs` but are mounted **read-only**. Our existing `MNT_RDONLY` filter already excludes them — but worth flagging so the future-us doesn't decide that all `apfs` mounts deserve a `tmutil` snapshot. A snapshot of a snapshot is not what we want.

**ZFS-on-macOS check:** Not installed on this dev host. OpenZFS-on-OSX ships its CLI at `/usr/local/bin/zfs` and `/usr/local/sbin/zfs` (varies by installer). When present, `zfs snapshot <pool>/<dataset>@<name>` is the canonical atomic-snapshot primitive — same consistency model as APFS via `tmutil`.

### Options considered

Three viable design shapes:

**A. Pre-filter by `f_fstypename` allowlist; run the right primitive per type.** Maintain an explicit table of `f_fstypename` → strategy. Strategies are: `snapshot+sync+fullfsync` (APFS, ZFS), `sync+fullfsync` (HFS, default), `sync-only` (FAT/exFAT/UDF if we want to avoid trying), `skip` (network, special, read-only). Pros: explicit, easy to reason about, log clearly differentiates. Cons: need to enumerate every type macOS can mount.

**B. Treat every writable local mount the same; tolerate `ENOTSUP` gracefully.** Skip network and special, then for everything else: try snapshot first if APFS, then try `F_FULLFSYNC`, accept failure as "best-effort flushed." Pros: doesn't need an exhaustive type allowlist; new filesystem types Just Work. Cons: less visibility into why a specific mount got which treatment.

**C. Hybrid — explicit deny-list for what to skip, generic try-everything-tolerantly for what's left.** Skip: network (`smbfs`, `afpfs`, `nfs`, `webdav`), special (`devfs`, `fdesc`, `volfs`, `synthfs`, `lifs`), placeholders (`autofs`), and `MNT_RDONLY`. For everything that passes the filter, run: if `apfs` (and not on the read-only Time Machine snapshot path) try `tmutil` snapshot once globally, then per-mount `sync()` already done at the top, then per-mount `F_FULLFSYNC` with `ENOTSUP` graceful skip; if `zfs` and `zfs` CLI present, prefer `zfs snapshot` over `F_FULLFSYNC`. Pros: explicit about what we *don't* touch (the dangerous decisions), generic about what we *do* (so we don't need to know every FS type a future macOS version might add). Cons: the report's per-treatment breakdown is computed at runtime rather than from a static table.

### Decision

**Option C** — hybrid: explicit deny-list for what to skip, generic-with-graceful-fallback for what passes the filter.

Reasoning: the dangerous decisions are about what *not* to touch (don't try to freeze a network mount; don't open an autofs placeholder; don't snapshot a read-only Time Machine backup volume). Those have to be deliberate. The decisions about *how* to flush a writable local volume are forgiving — `F_FULLFSYNC` already returns clean errors when unsupported, `tmutil` already returns the snapshot name on success, and a `sync()` at the top already flushed the dirty buffers. The Linux reference proves the "try-and-tolerate" model works for an analogous problem (FIFREEZE across mixed Linux FS types).

The deny-list also stays small and review-able. Every entry on it has a specific reason. New filesystem types macOS introduces in the future are automatically handled by the generic path with sensible defaults.

### Dispatch table

| `f_fstypename` / condition | Treatment | Counted as | Log level |
|---|---|---|---|
| `MNT_RDONLY` set | Skip | `skipped_readonly` | DEBUG |
| `smbfs`, `afpfs`, `nfs`, `webdav`, `ftp`, `osxfuse` (any network/userspace) | Skip | `skipped_network` | DEBUG |
| `autofs` | Skip | `skipped_special` | DEBUG (opening can trigger automount) |
| `devfs`, `fdesc`, `volfs`, `synthfs`, `lifs` | Skip | `skipped_special` | DEBUG |
| `apfs` AND mount path under `/Volumes/.timemachine/` | Skip | `skipped_readonly` | DEBUG (already RDONLY but defence in depth) |
| `apfs` (writable, not a Time Machine snapshot) | Snapshot-eligible (see below) + per-mount `F_FULLFSYNC` | `snapshotted` (if snapshot succeeded) + `fullfsynced` (per mount) | INFO on snapshot success |
| `zfs` (writable) AND `zfs` CLI present in `$PATH` | `zfs snapshot <ds>@<ts>` per pool/dataset; **do not** attempt `F_FULLFSYNC` | `zfs_snapshotted` | INFO on snapshot success |
| `hfs` (writable) | per-mount `F_FULLFSYNC` | `fullfsynced` | INFO |
| `msdos`, `vfat`, `exfat`, `udf` (writable) | per-mount `F_FULLFSYNC`, tolerate `ENOTSUP`/`EOPNOTSUPP` as "by design on this FS+version" | `fullfsynced` (on success) or `flushed_only` (on ENOTSUP) | INFO on success, DEBUG on ENOTSUP |
| `ntfs` (writable third-party R/W driver) OR anything else writable | per-mount `F_FULLFSYNC`, tolerate `ENOTSUP`/`EOPNOTSUPP` | `fullfsynced` or `flushed_only` | INFO on success, DEBUG on ENOTSUP |
| `F_FULLFSYNC` returns an unexpected error (not `ENOTSUP`/`EOPNOTSUPP`) | Count anyway via global `sync()` fallback | `flushed_only` + a single WARN | WARN (something *is* wrong but freeze still proceeds) |

**APFS snapshot scope:** `tmutil localsnapshot /` produces a snapshot of the boot volume's APFS container. It is **one snapshot per container**, not per mount. Today our `create_apfs_snapshot()` calls `tmutil localsnapshot /` (root only). With multiple APFS containers (e.g. an external APFS data volume on a separate disk), we'd need one `tmutil localsnapshot` per container root. For now: keep the single-snapshot behaviour, but note in the response that snapshot covers the *boot container only*; per-mount `F_FULLFSYNC` is what covers writable non-boot mounts. A future enhancement: enumerate distinct APFS containers (`diskutil apfs list -plist`) and snapshot each. Out of scope for this Phase 2 unless someone has a concrete multi-container case.

**ZFS:** If `zfs` is in `$PATH`, prefer `zfs snapshot` for ZFS mounts over `F_FULLFSYNC`. Pool/dataset name derived from `df -P` output (the `Filesystem` column for ZFS mounts is `pool/dataset` form). Snapshot name pattern: `mac-guest-agent-<utc-iso8601>`. On freeze: `zfs snapshot <pool>/<dataset>@<name>`. On thaw: `zfs destroy <pool>/<dataset>@<name>` (mirrors how we clean up APFS snapshots). Failure of `zfs snapshot` is logged WARN and the mount falls through to `F_FULLFSYNC` (defence in depth) rather than failing the whole freeze.

### Failure modes — must-surface vs by-design

**Must surface (WARN or higher):**

- A writable mount's `F_FULLFSYNC` returns an error code that is *not* `ENOTSUP`/`EOPNOTSUPP`. Examples: `EIO` (drive failure), `EAGAIN` (kernel under pressure), `EACCES` (permissions broken). These are real problems and the operator needs to know.
- The global `sync()` returns non-zero. Should never happen on a healthy system; if it does the freeze is unreliable.
- `tmutil localsnapshot` fails. We continue with `F_FULLFSYNC` (defence in depth) but the operator loses the atomic-snapshot consistency point and should be told.
- `zfs snapshot` fails on a ZFS mount where the CLI was present. Same reasoning as `tmutil` failure.
- Zero volumes were touched in a freeze attempt — the response would be `0` and `freeze_status` would be unset per upstream pattern. Currently we accept this silently; it should be a WARN at minimum because something is wrong with the mount enumeration.

**By-design — must NOT look alarming:**

- `F_FULLFSYNC` returning `ENOTSUP`/`EOPNOTSUPP` on FAT/exFAT/UDF on macOS versions whose driver doesn't implement it (Tiger-era). DEBUG log only; volume counted as `flushed_only` via the global `sync()`.
- Network mounts (`smbfs`, `nfs`, `afpfs`, `webdav`) being skipped. DEBUG log only.
- Special kernel filesystems (`devfs`, `fdesc`, etc.) being skipped. DEBUG log only.
- Read-only mounts (including Time Machine local snapshot mounts under `/Volumes/.timemachine/`) being skipped. DEBUG log only.
- `autofs` placeholders being skipped. DEBUG log only.

### Implementation implications

Changes flow into:

- **`src/cmd-fs.c sync_all_volumes()`** — replace the current uniform loop with the dispatch table above. Track per-treatment counters: `snapshotted`, `zfs_snapshotted`, `fullfsynced`, `flushed_only`, `skipped_network`, `skipped_special`, `skipped_readonly`. Return either the existing single `int` (sum of the four "did-something" counters) or a new struct depending on Question 3's decision. Log a single INFO line summarising the per-treatment breakdown at the end of the loop.
- **`src/cmd-fs.c create_apfs_snapshot()`** — unchanged for the boot-container case. Add a TODO note about multi-container enumeration as a future enhancement.
- **New helper:** `fs_dispatch_class(const struct statfs *mnt)` returning an enum of `{SKIP_RDONLY, SKIP_NETWORK, SKIP_SPECIAL, APFS_WRITABLE, ZFS_WRITABLE, HFS_WRITABLE, GENERIC_WRITABLE}`. All filter decisions in one place; easy to unit-test against canned `statfs` inputs.
- **ZFS detection helper:** check `access("/usr/local/bin/zfs", X_OK)` or `access("/usr/local/sbin/zfs", X_OK)` once at startup; cache the result. Only act on ZFS mounts if the CLI is present.
- **`docs/BACKUP.md` (or a new `docs/design/FREEZE_SEMANTICS.md`)** — publish the dispatch table as user-facing documentation. Honest about what "freeze" means per filesystem type on macOS.

Tests:

- **Mock-driven unit test for `fs_dispatch_class`** in `tests/test_unit.c`: synthetic `struct statfs` inputs covering each row of the table, assert the returned class. No real filesystems touched.
- **Existing freeze integration test** in `tests/test_proactive.c` continues to validate the end-to-end dry-run path; once Question 3 settles the response shape, update the test's expectations.

---

## Question 2 — `guest-fsfreeze-freeze-list` mountpoints argument

**Status:** to be designed.

### The question

QGA spec defines `guest-fsfreeze-freeze-list` with an optional `mountpoints: [str]` argument — freeze *only* the listed mountpoints, leave the rest writable. We currently ignore the argument because we register both `freeze` and `freeze-list` against the same `(void)args` handler. A caller passing `mountpoints` gets a global freeze instead of a subset freeze.

Options:
- Implement subset behaviour (parse `args.mountpoints`, only operate on listed mounts).
- Unregister `guest-fsfreeze-freeze-list` (don't claim to support what we don't).

### Decision

_(to be filled)_

---

## Question 3 — Honest per-volume reporting in the freeze response

**Status:** to be designed.

### The question

Today the response is a single integer `frozen_volume_count`. With Question 1's per-FS dispatch, that integer is structurally misleading — 1 APFS snapshot, 1 HFS+ F_FULLFSYNC, and 1 FAT32 sync-only are different operations with different guarantees.

Options:
- Keep the int as the wire response; surface the per-FS breakdown in the log line only.
- Add a structured per-FS-treatment breakdown to the response as a non-spec extension.
- Stick with the int as wire response and report the breakdown only via `--safe-test-json` / `--self-test-json`.

### Decision

_(to be filled)_

---

## Question 4 — `guest-get-cpustats` shape

**Status:** to be designed.

### The question

Our response shape `{user, system, idle, nice}` is not spec-conformant. The QGA spec defines `['GuestCpuStats']` as a per-CPU array of discriminated-union structs, gated on `CONFIG_LINUX`.

Options:
- Extend the discriminated union with a `darwin` variant; produce per-CPU rows via `HOST_PROCESSOR_INFO` / `PROCESSOR_CPU_LOAD_INFO`.
- Stop registering the command on macOS (it's Linux-only in upstream).
- Keep the current shape, document it as a deliberate macOS extension.

### Decision

_(to be filled)_

---

## Question 5 — Command-gating allowlist alignment with upstream

**Status:** to be designed.

### The question

Upstream's allowlist (6 commands) blocks `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` once frozen — only `thaw` exits the state. Ours (9) allows idempotent re-freeze. Also: should `guest-get-fsinfo` (read-only, safe during freeze) be added to allowed?

Options:
- Keep idempotent re-freeze; document the divergence.
- Align with upstream's strict posture.
- Add specific commands like `get-fsinfo` to the allowlist if they're safe.

### Decision

_(to be filled)_

---

## Question 6 — Frozen-state persistence and logging during freeze

**Status:** to be designed.

### The question

Upstream writes a persistent on-disk marker so a crashed agent detects prior-frozen state on restart, and disables logging to avoid writing to a frozen volume. We do neither.

Options:
- Adopt both: write the marker, disable logging while frozen.
- Adopt the marker only (our log path may or may not be on a frozen volume; logging-during-freeze rarely deadlocks because our freeze isn't a true I/O suspension).
- Document the divergence explicitly and don't change behaviour.

### Decision

_(to be filled)_

---

## Question 7 — Documentation honesty

**Status:** to be designed.

### The question

Phase 1 surfaced three misleading or oversimplified claims in our docs:

1. `docs/PVE.md` "Accurate Memory Reporting Without Balloon Driver" implies our agent improves PVE's UI memory gauge. It doesn't — PVE reads cgroup RSS for macOS guests regardless of our agent.
2. `README.md` / `docs/COMPATIBILITY.md` "ISA because Apple claims VirtIO" is right for Apple Virtualization.framework hosts but oversimplified for QEMU/OpenCore, where Apple's QGA never launches.
3. The user-facing meaning of "freeze" on macOS — needs an explicit per-`f_fstypename` table once Question 1 is implemented.

### Decision

_(to be filled)_

---

## Decisions summary

_(populated as questions are answered)_

| # | Question | Decision |
|---|---|---|
| 1 | Per-FS freeze strategy | **Option C** (deny-list for skip; generic try-with-tolerate for the rest; ZFS via `zfs snapshot` if CLI present; APFS via `tmutil` + `F_FULLFSYNC` defence in depth). Full dispatch table in Q1. |
| 2 | freeze-list mountpoints | _(pending)_ |
| 3 | Freeze response shape | _(pending)_ |
| 4 | cpustats shape | _(pending)_ |
| 5 | Allowlist alignment | _(pending)_ |
| 6 | Frozen-state persistence | _(pending)_ |
| 7 | Documentation honesty | _(pending)_ |

## Implementation queue

_(populated as decisions land — each becomes one or more commits)_
