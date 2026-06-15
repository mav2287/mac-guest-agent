# Agent behaviour specification

**Status:** historical reference (Phase 2 of the design process that produced v2.4.3). The decisions documented below have shipped; the file is preserved for design-history context, not as a description of work in progress. See CHANGELOG v2.4.3 for the implementation summary.

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

### The question

QGA spec defines `guest-fsfreeze-freeze-list` with an optional `mountpoints: [str]` argument — freeze *only* the listed mountpoints, leave the rest writable. Typical use: a backup tool wants to freeze a data volume but keep the OS volume writable so the backup tool's own logs can land somewhere. **(v2.4.3+ implementation note: the mountpoints filter is now wired through `src/cmd-fs.c handle_fsfreeze_freeze_list()` — see CHANGELOG. The question text below is preserved for design-history context; the answer shipped.)** The original pre-fix state: we ignored the argument because we registered both `freeze` and `freeze-list` against the same `(void)args` handler.

### Evidence

- Phase 1 Target 1: spec is `{ '*mountpoints': ['str'] }` — optional array.
- Phase 1 Target 2: the Linux reference (`commands-linux.c:qmp_guest_fsfreeze_do_freeze_list`) implements it as `if (has_mountpoints) { ... skip if not in list ... }` — straightforward filter in the iteration loop.
- PVE's `guest_fs_freeze` helper calls `guest-fsfreeze-freeze` (not `freeze-list`). The PVE backup path doesn't exercise this command.
- No evidence anyone currently calls `freeze-list` with mountpoints against our agent.

### Options considered

- **Implement subset behaviour.** Parse `args.mountpoints` (a JSON string array), pass to the dispatch loop, only operate on mounts whose path matches one of the listed entries. Aligns with spec; future-callers Just Work.
- **Unregister.** Remove from `command_register`. Callers get "command not supported," fall back to `freeze` or fail explicitly.

### Decision

**Implement subset behaviour.**

Cost is bounded — perhaps 30–50 lines: parse the string array from `cJSON`, pass a pointer-to-list into the new `sync_all_volumes_filtered()` helper Q1 will introduce, skip any mount whose `f_mntonname` isn't in the list. The dispatch table from Q1 still applies per mount, the per-treatment counters still work, the response is still the int count of "did-something" volumes.

The value is twofold: (a) spec conformance — we stop silently doing the wrong thing on a command we advertise; (b) future-proof — if any backup tooling that targets the QGA spec ever runs against our agent, partial freeze works correctly. The cost is small enough that "unregister" feels like sweeping a real gap under the rug.

### Implementation implications

- `src/cmd-fs.c` — replace the shared registration:
  ```c
  command_register("guest-fsfreeze-freeze-list", handle_fsfreeze_freeze_list, 1);
  ```
  with a distinct handler `handle_fsfreeze_freeze_list` that:
  1. Parses `args.mountpoints` as a `cJSON` string array.
  2. If absent or empty array → delegate to `handle_fsfreeze_freeze` (global freeze).
  3. If present and non-empty → build a `const char *const *` whitelist and call `sync_all_volumes_filtered(do_fullfsync=1, mountpoints=whitelist)`.
- `src/cmd-fs.c sync_all_volumes()` (renamed/wrapped) — add an optional `mountpoints` filter parameter. If non-NULL, skip any mount whose `f_mntonname` isn't in the list. The per-FS dispatch (Q1) still runs for the volumes that pass the filter.
- The "set frozen state before the loop, unset if zero touched" sequencing from upstream (Phase 1 Target 2) applies here too.
- Add a unit test in `tests/test_proactive.c`: dry-run freeze-list with `["/Volumes/data"]` → only the data mount is "touched" (in dry-run we just verify the dispatch decision); freeze-list with `[]` → all mounts touched (delegates to plain freeze).

### Failure modes

- Caller passes a path that isn't mounted on the guest → the path is silently absent from the iteration; the response int is the number of *listed* mounts that actually got operated on. If zero matched, response is `0` (caller can detect: "I asked for 2 mounts, got 0 frozen"). Same as upstream's behaviour.
- `args.mountpoints` is the wrong type (e.g. string instead of array) → return spec-shaped error `{class: "GenericError", desc: "mountpoints must be an array of strings"}`.

---

## Question 3 — Honest per-volume reporting in the freeze response

### The question

Today the freeze response is a single integer `frozen_volume_count`. With Q1's per-FS dispatch, that integer is structurally misleading — 1 APFS snapshot + 1 HFS+ F_FULLFSYNC + 1 FAT32 sync-only-via-ENOTSUP-fallback are three different operations with three different guarantees, all summed into "3."

### Evidence

- Phase 1 Target 1: spec for `guest-fsfreeze-freeze` and `-freeze-list` is `'returns': 'int'`. Strict. Anything other than an int violates spec.
- Phase 1 Target 2: Linux reference returns "count of FIFREEZE calls that succeeded" — already an apples-to-apples sum (since FIFREEZE is uniform across Linux FS types). Their int is meaningful; ours becomes less so under per-FS dispatch.
- Phase 1 Target 4: PVE's `guest_fs_freeze` does `mon_cmd($vmid, 'guest-fsfreeze-freeze', timeout => $timeout)` and checks for an error response or numeric return — it doesn't inspect any extension fields. PVE's tolerance for extra response fields is the standard QMP "ignore unknown keys" pattern (likely tolerant, but not contractually so).

### Options considered

- **A. Keep int wire response; log breakdown only.** Spec-conformant. The structured detail lives only in `/var/log/mac-guest-agent.log`, which isn't reachable from the host without an out-of-band fetch.
- **B. Extend wire response with a structured breakdown** (e.g. `{"return": {"count": 3, "by_treatment": {...}}}`). Breaks spec — the contract says `int`, we'd be returning an object. Most clients ignore unknown fields but strict ones may reject.
- **C. Keep int wire response; emit a structured INFO log line; surface breakdown via `--safe-test-json` / `--self-test-json` / `pve-verify.sh` log fetch.**

### Decision

**Option C.**

Wire response stays `int` — the sum of "did-something" counters (`snapshotted + zfs_snapshotted + fullfsynced + flushed_only`). Spec-conformant. PVE's `qm guest cmd fsfreeze-freeze` continues to see the integer it expects.

Per-treatment detail surfaces in three places:

1. **Agent log, INFO level, single line per freeze event:**
   ```
   Freeze complete: 1 snapshotted, 0 zfs_snapshotted, 2 fullfsynced, 1 flushed_only, 4 skipped (2 network, 1 special, 1 readonly)
   ```
2. **`--self-test-json` env diagnostic** — extend the `freeze` section to expose: (a) the dispatch table this build implements, (b) the log file path so external tooling knows where to fetch the per-event line. Doesn't actually run a freeze — just states what would happen.
3. **`pve-verify.sh`** — after the freeze round-trip, use `qm agent <vmid> exec` to `tail -n 50 <log_path> | grep "Freeze complete"` and embed the line in the host-side report. This is the path Phase 3 will wire up.

This keeps the spec contract clean (PVE and any other QGA consumer gets the int they expect) while giving contributors and operators full visibility into what actually happened. The `--self-test-json` static description means anyone can see the dispatch policy even without running a freeze.

### Implementation implications

- `src/cmd-fs.c handle_fsfreeze_freeze()` / `handle_fsfreeze_freeze_list()` — keep returning `cJSON_CreateNumber(frozen_volume_count)`. No wire-shape change.
- `src/cmd-fs.c sync_all_volumes()` — populate a stack-allocated struct of counters; log a single INFO line with the full breakdown at the end of the freeze loop.
- `src/selftest.c emit_system_info()` — add a `freeze_dispatch` object summarising the per-FS treatment table and the log file path. Static description, no real freeze run.
- `scripts/pve-verify.sh` (Phase 3 task) — after `qm guest cmd <vmid> fsfreeze-freeze`, run `qm agent <vmid> exec --path tail --arg -n50 --arg /var/log/mac-guest-agent.log`, poll exec-status, base64-decode, grep for the `Freeze complete` line, embed in report.

### Failure modes

- Log file unreadable / missing → `pve-verify.sh` reports "freeze breakdown unavailable (log path: ...)" and continues; the int response still tells the operator the rollup count.
- Log file rotated mid-test → the breakdown line may not appear in the most recent N lines; fall back to "breakdown unavailable, last known: <previous run>." Not a freeze failure, just a reporting gap.

---

## Question 4 — `guest-get-cpustats` shape

### The question

Our response is a flat aggregate object `{user, system, idle, nice}` for a command the QGA spec defines as `['GuestCpuStats']` — a list of discriminated-union per-CPU structs with a `type` discriminator (enum `GuestCpuStatsType`, currently only `linux`), gated on `CONFIG_LINUX`. Three paths:

- Extend the discriminated union with a `darwin` variant.
- Stop registering `guest-get-cpustats` on macOS.
- Keep current shape, document as a deliberate macOS extension.

### Evidence

- Phase 1 Target 1: spec is `CONFIG_LINUX`-only with the `GuestLinuxCpuStats` variant. There is no `darwin` enum value in the upstream `GuestCpuStatsType`.
- Phase 1 Target 5: PVE's `vmstatus()` reads CPU% from `/proc/<qemu-pid>/stat`. It does NOT call `guest-get-cpustats`. Our shape doesn't affect any PVE UI.
- Phase 1 Target 6: Apple's built-in QGA does not implement `guest-get-cpustats` at all.
- Faking `type: "linux"` from a non-Linux guest would slip past spec parsers but lie about the source OS — a misrepresentation that a strict consumer could detect (e.g. by cross-referencing with `get-osinfo`).
- Adding `type: "darwin"` to the union requires upstream cooperation that doesn't exist; a strict consumer would reject unknown discriminator values.

### Options considered

- **A. Extend union with `darwin` variant + produce per-CPU rows.** Requires using `host_processor_info(PROCESSOR_CPU_LOAD_INFO, ...)` instead of the aggregate `host_statistics(HOST_CPU_LOAD_INFO)`. Returns the spec's expected list shape; honest about being from a Darwin host; cannot be parsed by a Linux-only client that ignores unknown discriminator values. Out-of-spec in the sense that the upstream enum doesn't list `darwin`.
- **B. Fake `type: "linux"`.** Spec-parseable; lies about the source OS. Easy to detect via cross-referencing.
- **C. Stop registering on macOS.** Honest; clients that try `get-cpustats` get "command not supported"; they fall back to whatever (likely QMP host-side, the same path PVE already uses).
- **D. Keep current flat aggregate; document as a deliberate extension.** Comfortable but spec-violating in a way that silently breaks any client that follows the spec strictly.

### Decision

**Fix the shape to spec-conformant per-CPU array; use `type: "linux"` as the discriminator.**

Reasoning, drawing on Phase 1 Targets 1, 5, 6 and follow-on parser-behaviour analysis:

- The current flat aggregate object is structurally invalid against the spec at the array level (spec returns `['GuestCpuStats']` — a list, not an object). Anything we change to is an improvement; the question is which improvement.
- Looking at parser behaviour for each option:
  - `type: "linux"` — accepted by strict QAPI parsers (known enum value), accepted by lax parsers, field semantics (`user`/`nice`/`system`/`idle` ticks) translate correctly from macOS's `HOST_PROCESSOR_INFO` to Linux's `GuestLinuxCpuStats`. Zero parser-rejection risk.
  - `type: "darwin"` — rejected by strict QAPI parsers (unknown enum value of `GuestCpuStatsType`), accepted by lax parsers. Honest about source OS but creates a parser-failure risk for the strict set.
  - No `type` field — rejected by strict QAPI parsers (the field is part of the union's `base` and is required by schema), accepted by lax parsers. *More* severe spec violation than `type: "darwin"` because it's a missing required field, not an unknown enum value.
- Cross-platform precedent confirms we're not setting a bad pattern: FreeBSD QGA and Windows QGA both **decline to register** `guest-get-cpustats` (the upstream `'if': 'CONFIG_LINUX'` gate compiles it out on non-Linux builds). There is no precedent anywhere for `type: "darwin"`. If we wanted to be that precedent we'd need upstream collaboration to add the enum value — which is deferred (see PLAN.md note on possible future work).
- PVE doesn't consume the command (Phase 1 Target 5). Strictly speaking, any of the four options work in PVE's lax-parser world. We pick `type: "linux"` because it's safe for the strict-parser fraction too at zero additional cost.

The honest disclosure of this choice lives in the source-file comment and in the `--self-test-json` env block:

```c
/* Use type:"linux" rather than "darwin": the QGA spec's GuestCpuStatsType
 * enum currently has no "darwin" value, and emitting one would be rejected
 * by strict QAPI parsers. The user/system/idle/nice tick semantics translate
 * correctly from macOS's HOST_PROCESSOR_INFO to Linux's struct, so the field
 * shape is honest even if the discriminator names the spec-defined variant.
 * If upstream QEMU ever adds a "darwin" variant to GuestCpuStatsType
 * (see PLAN.md "possible future work"), switch to it then. */
```

### Implementation implications

- `src/cmd-hardware.c handle_get_cpustats()` — rewrite to:
  1. Call `host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpu_count, &info, &info_count)` to get per-CPU tick arrays (Apple API present since 10.0 — works on every supported version).
  2. Build a `cJSON_CreateArray()` and append one object per CPU containing `{type: "linux", cpu: N, user: ticks, nice: 0, system: ticks, idle: ticks}`.
  3. `nice` ticks: macOS's `host_processor_info` doesn't separate nice from user; report `nice: 0` and accept that niced time rolls into `user`. Document in the code comment.
  4. Free the Mach buffer via `vm_deallocate(mach_task_self(), (vm_address_t)info, info_count * sizeof(integer_t))`.
- Keep the registration in `cmd_hardware_init()` — same command name, fixed shape.
- `--self-test-json` env block (`emit_system_info()` in `src/selftest.c`) — add a brief note describing the cpustats shape and the `type:"linux"` discriminator-choice rationale, so anyone tracing where the `"linux"` came from finds the explanation without having to read source.
- No removal from `docs/COMMAND_STATUS.md` for this (cpustats) decision; it leaves the count unchanged. (The overall registered count is now **42**, not 45 — `guest-fstrim` was later removed; see RECLAIM.md.)
- No test changes needed — `--safe-test` doesn't currently exercise `guest-get-cpustats` (verified in `src/selftest.c:626-627`: the array has `Memory block size` and `Memory blocks`, no cpustats entry).

### Failure modes

- `host_processor_info` returns non-zero (Mach API failure) → return the existing error path: `{class: "GenericError", desc: "Failed to get per-CPU statistics"}`. Caller sees a spec-shaped error response.
- Strict QAPI consumer encounters the response → accepts cleanly because `type: "linux"` is a valid discriminator and the field set matches `GuestLinuxCpuStats`'s required fields.
- Lax consumer encounters the response → accepts cleanly; field shapes are what every QGA spec consumer already expects from Linux guests.
- An operator examines a response from a macOS guest and notices `type: "linux"` → the in-source comment and the `--self-test-json` env note both explain the deliberate choice and the upstream path that would make it `"darwin"` honestly.

---

## Question 5 — Command-gating allowlist alignment with upstream

### The question

Upstream's freeze-allowed list (6 commands) blocks `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` once frozen — only `thaw` exits the state. Ours (9) allows idempotent re-freeze. The deeper question: which of our 42 commands should be allowed during freeze? Current 9 was set heuristically.

### Evidence

- Phase 1 Target 3: upstream's allowlist:
  ```
  guest-ping, guest-info, guest-sync, guest-sync-delimited,
  guest-fsfreeze-status, guest-fsfreeze-thaw
  ```
  Upstream's rationale (implied by the strict posture): on a real Linux freeze, FS reads can block if the FS implementation doesn't service them during the freeze window. Conservative posture protects the freeze contract.
- Our pseudo-freeze (sync + F_FULLFSYNC + optional APFS/ZFS snapshot) does **not** suspend I/O. Reads-during-freeze are functionally unaffected.
- Upstream blocks re-freeze; we accept it idempotently. The benefit of idempotence: a backup tool that retries on timeout doesn't have to thaw-then-refreeze. The cost of upstream's strictness: callers must implement explicit thaw before retry.

### Options considered

- **A. Align with upstream's 6.** Maximum conformance; loses idempotent re-freeze (regression for any caller that depended on it — likely none, but possible) and blocks read-only inspection during freeze (annoying for backup tools that want to query state).
- **B. Keep current 9.** Status quo. The read-only "get" commands remain blocked, including useful ones like `guest-get-fsinfo`, `guest-get-osinfo`, `guest-network-get-interfaces`.
- **C. Adopt a principled rule: allow during freeze iff handler is read-only, doesn't execute external programs, doesn't change agent state except via the freeze-status flag.** Enumerate the resulting allowlist (~27 commands). Larger than upstream's 6, smaller than the full 44. Documented divergence.

### Decision

**Keep the current 9-command allowlist. No expansion. Document the principled-restrictive rule and the deliberate divergences from upstream.**

The 9 commands stay as they are:

```
Protocol / control:
  guest-ping
  guest-sync
  guest-sync-id          (our extension; harmless during freeze)
  guest-sync-delimited
  guest-info

Freeze control:
  guest-fsfreeze-status
  guest-fsfreeze-freeze        (idempotent — divergence from upstream)
  guest-fsfreeze-freeze-list   (idempotent — divergence from upstream)
  guest-fsfreeze-thaw
```

Reasoning, drawing on the Phase 1 Linux/Windows/BSD comparison:

- The upstream freeze allowlist in `qga/main.c` is **6 commands** (`ping`, `info`, `sync`, `sync-delimited`, `fsfreeze-status`, `fsfreeze-thaw`) and is **shared across Linux, Windows, and BSD builds** — same list applies regardless of underlying freeze primitive. Upstream calibrated for the most restrictive case (Linux FIFREEZE, a true I/O suspension) and Windows VSS / BSD UFSSUSPEND inherit the conservatism.
- Our 9 are a strict superset of upstream's 6: we add `guest-sync-id` (our extension command, harmless) and we allow idempotent re-entry into the frozen state via `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list`. The idempotent re-freeze is a deliberate divergence — our handler at `src/cmd-fs.c:286-288` returns the current `frozen_volume_count` if already frozen without any double-freeze damage, which benefits backup tools that retry on timeout.
- Earlier we considered expanding to 28 commands (all `guest-get-*`, read-only file ops, `exec-status`, etc.) on the reasoning that our pseudo-freeze isn't a true I/O suspension and reads are functionally safe during it. **Rejected** for three reasons:
  1. No current consumer demand. PVE only calls `freeze`/`status`/`thaw` during the freeze window. We have zero evidence anyone wants `get-osinfo` or `network-get-interfaces` mid-freeze.
  2. Per-FS-type allowlists would be even worse: APFS-snapshot freezes (consistency point captured) could in principle allow more than HFS+-flush-only freezes (no consistency point if writes happen), but tracking which kind of freeze we're in and dispatching the allowlist accordingly adds significant state-machine complexity for unclear benefit.
  3. Conservative default is safe regardless of underlying semantics; permissive default is only safe if we can guarantee the semantics support it. The asymmetry favours conservative.

If a real use case emerges later that needs specific commands added (a backup tool that genuinely benefits from querying `get-fsinfo` mid-freeze, for example), expand at that point with the concrete justification.

### Implementation implications

- **No code change.** `src/cmd-fs.c fsfreeze_command_allowed()` keeps its current 9-entry static array.
- Update the function's comment to state the principled-restrictive rule and link to this spec doc:
  ```c
  /* Commands allowed during freeze. Conservative-by-default per the design
   * spec (see docs/design/AGENT_BEHAVIOUR_SPEC.md, Question 5): only
   * protocol-level commands, freeze control (idempotent re-freeze accepted
   * — deliberate divergence from upstream which blocks re-freeze), and
   * thaw. Read-only inspection commands are NOT allowed during freeze
   * unless a concrete use case justifies expansion. */
  ```
- One small clarifying assert in unit tests (`tests/test_proactive.c`): exercise `fsfreeze_command_allowed` with representative commands from each category to lock the contract.

### Failure modes

- A caller that targets upstream's strict 6-command allowlist works fine — everything they'd send is in our allowed set.
- A caller that expected upstream's blocked-re-freeze behaviour and sends `freeze` twice gets a successful response both times (the idempotent divergence). Documented in this spec.
- A new command added in the future is implicitly blocked during freeze (the allowlist is an enumerated array, not derived from a flag). Safe default. Any PR that adds a new command needing freeze-time access must update this allowlist explicitly and justify it.

---

## Question 6 — Frozen-state persistence and logging during freeze

### The question

Upstream writes a persistent on-disk marker (`s->state_filepath_isfrozen`) so a crashed agent detects prior-frozen state on restart, and disables logging to avoid writing to a frozen volume. We do neither.

### Evidence

- Phase 1 Target 3: upstream's `ga_set_frozen` does `ga_create_file(s->state_filepath_isfrozen)`, `ga_disable_logging(s)`, plus the freeze-state flag.
- Upstream's logging-disable rationale: writing to `/var/log/...` during a real freeze deadlocks if the log volume is the frozen volume. Their freeze is a true I/O suspension.
- Our pseudo-freeze does **not** suspend I/O. Log writes during freeze proceed normally. No deadlock.
- Our `create_apfs_snapshot()` already handles orphan cleanup (`cmd-fs.c:232-235`): on a fresh freeze, if `snapshot_date[0]` is non-empty (a leftover from a previous run), it calls `delete_apfs_snapshot()` before creating a new one. So the only piece of "state lost on crash" — an orphaned snapshot — is already cleaned up on the next freeze.

### Options considered

- **A. Adopt both** (write marker, disable logging while frozen).
- **B. Adopt marker only.**
- **C. Document divergence; don't change behaviour.**

### Decision

**Option C — document divergence, don't change behaviour.**

Reasoning:

- **Marker:** the only piece of crash-time state that matters for us is the orphaned APFS snapshot. Our existing `create_apfs_snapshot()` already cleans up the orphan on the next freeze. Adding a separate persistent marker adds another piece of state to manage (write, read, delete on thaw, handle if disk full, etc.) without solving a problem we have.
- **Logging disable:** our pseudo-freeze doesn't deadlock on logging. The upstream protection is for a problem we don't have.
- **Cost of adoption:** real (new code paths, new failure modes — what if marker can't be written? what if log is reopened mid-freeze?). Benefit: marginal.

Document the divergence in `docs/design/FREEZE_SEMANTICS.md` (created as part of Q7) with the reasoning: "our freeze is not a true I/O suspension, so the upstream protections aren't needed."

### Implementation implications

- No code change. This is the only Phase 2 question whose decision is "do nothing."
- `docs/design/FREEZE_SEMANTICS.md` (created by Q7) gets a "Divergences from QEMU's reference QGA" section noting (a) idempotent re-freeze (Q5) and (b) no persistent frozen-state marker / no logging-disable-during-freeze (this question), with the rationale.

### Failure modes

- Agent crashes mid-freeze on APFS → restarts with `freeze_status=0`. The orphaned APFS snapshot remains until the next freeze, which cleans it up. The intervening time it costs disk space proportional to the writes that happened between the snapshot and the crash. Acceptable.
- Agent crashes mid-freeze on HFS+ / FAT / etc. → restarts with `freeze_status=0`. No state to clean up; F_FULLFSYNC has no persistent side effect. Clean.
- Agent crashes mid-freeze on ZFS (if we added it per Q1) → orphaned `zfs snapshot` remains until next freeze; same handling as APFS. We should mirror APFS's cleanup pattern: on `create_zfs_snapshot()`, if a previous snapshot name is tracked, `zfs destroy` it before creating the new one.

---

## Question 7 — Documentation honesty

### The question

Phase 1 surfaced three misleading or oversimplified claims in our docs:

1. `docs/PVE.md` "Accurate Memory Reporting Without Balloon Driver" implies our agent improves PVE's UI memory gauge. It doesn't — PVE reads cgroup RSS for macOS guests regardless of our agent (Phase 1 Targets 5 and 7).
2. `README.md` and `docs/COMPATIBILITY.md` "ISA because Apple claims VirtIO" is right for Apple Virtualization.framework hosts but oversimplified for QEMU/OpenCore, where Apple's QGA never launches (Phase 1 Target 6).
3. The user-facing meaning of "freeze" on macOS — needs an explicit per-`f_fstypename` table once Q1's dispatch is implemented.

### Decision

**All three revisions. Specifically:**

#### 7a — Rewrite the memory-reporting claim

Target: the "Accurate Memory Reporting Without Balloon Driver" section in `docs/PVE.md` (and any analogous claim in `README.md`).

Replace with text along these lines:

> **Memory reporting on macOS guests**
>
> Proxmox's per-VM memory gauge for macOS guests reflects the QEMU process's host-side memory footprint (cgroup RSS), not the guest's view of its own RAM usage. This is a structural limitation: the gauge sources memory stats from the virtio-balloon device, which requires a guest-side driver that ships with most Linux distributions but does not exist on macOS. Apple has never shipped a virtio-balloon driver, and the protocol has no host-pull alternative.
>
> Our agent provides the *guest's* actual memory view via `guest-get-memory-blocks` and `guest-get-memory-block-info`. PVE's web UI doesn't consume these (it doesn't query the guest agent for memory), but you can read them directly:
>
> ```bash
> qm agent <vmid> get-memory-block-info   # block size
> qm agent <vmid> get-memory-blocks        # block list (used = online * size)
> ```
>
> `scripts/pve-verify.sh` translates these into a human-readable "~X GB used / ~Y GB total" report.

Avoids the word "accurate" entirely — replaces it with a description of what we actually provide and the path to consume it.

#### 7b — Refine the ISA rationale

Target: the "ISA Serial Transport" sections in `README.md`, `docs/COMPATIBILITY.md`, and the comment in `src/channel.c:31-42`.

Replace the current "ISA because Apple claims VirtIO" framing with:

> **Why ISA serial**
>
> macOS can run as a guest in two distinct ways: under **Apple's Virtualization.framework** (UTM, `vz_run`, anything backed by `VZVirtualMachine`), and under **plain QEMU/KVM** (Proxmox, libvirt, raw QEMU, often with OpenCore as the bootloader).
>
> On Virtualization.framework hosts, macOS detects the VZ environment via the `AppleVirtIOAgentDevice` IOKit property and Apple's built-in `AppleQEMUGuestAgent` launches on the VirtIO console channel. Our agent on the VirtIO channel would conflict with Apple's.
>
> On Proxmox/QEMU/OpenCore hosts, the `AppleVirtIOAgentDevice` property is not set and Apple's agent never launches — technically we could use the VirtIO channel there. We use ISA serial universally for consistency: one transport across both host types, no host-detection logic, no per-environment conditional registration. The trade-off is documented at the cost of a slightly older transport that all macOS versions from 10.4 onwards support natively via `Apple16X50Serial.kext`.

#### 7c — Per-FS freeze semantics doc

Create `docs/design/FREEZE_SEMANTICS.md` (linked from `docs/BACKUP.md` and the `README.md` features list). Contents:

- "macOS has no FIFREEZE" — short intro explaining the absence of a kernel-level filesystem-freeze primitive on macOS, contrasted with Linux's FIFREEZE and FreeBSD's UFSSUSPEND.
- "What 'freeze' means here" — a sentence per filesystem class summarising the dispatch table from Q1.
- The Q1 dispatch table itself (verbatim).
- "What 'frozen' status guarantees" — the agent reports `frozen` when at least one volume was successfully snapshotted or F_FULLFSYNC'd. The freeze is best-effort flush, not I/O suspension; reads continue, writes that happened before freeze are guaranteed on physical media (for filesystems that support F_FULLFSYNC), writes that happen after freeze are NOT prevented.
- "Divergences from QEMU's reference QGA" — the Q5 and Q6 divergences with reasoning.
- "Failure modes" — must-surface vs by-design, from Q1.

### Implementation implications

- Three doc files updated (`docs/PVE.md`, `README.md`, `docs/COMPATIBILITY.md`), one source file comment updated (`src/channel.c`), one new doc created (`docs/design/FREEZE_SEMANTICS.md`).
- All doc changes can land in one commit at the end of Phase 2's implementation queue, *after* Q1's code changes land — because the per-FS table in `FREEZE_SEMANTICS.md` should match what's in the code, not what's aspirational.

### Failure modes

- N/A — documentation changes don't have runtime failure modes. The risk is the doc going stale relative to code; addressed by keeping the per-FS table in `FREEZE_SEMANTICS.md` aligned with the dispatch helper `fs_dispatch_class()` (one source of truth: the code; the doc cites it).

---

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

| # | Question | Decision |
|---|---|---|
| 1 | Per-FS freeze strategy | **Hybrid:** explicit deny-list for skip categories (network / special / autofs / RDONLY); generic try-`F_FULLFSYNC`-with-`ENOTSUP`-tolerance for the rest. APFS gets `tmutil localsnapshot` + `F_FULLFSYNC` (defence in depth). ZFS gets `zfs snapshot` (if `zfs` CLI present), else falls through to `F_FULLFSYNC`. Per-treatment counters: `snapshotted`, `zfs_snapshotted`, `fullfsynced`, `flushed_only`, `skipped_network`, `skipped_special`, `skipped_readonly`. |
| 2 | `freeze-list` mountpoints | **Implement subset behaviour.** Distinct handler parses `args.mountpoints`; filters the dispatch loop; same per-FS strategy per Q1; same counters; same wire response. |
| 3 | Freeze response shape | **Keep `int` wire response (spec-conformant).** Per-treatment breakdown surfaces in (a) the agent log INFO line, (b) `--self-test-json`'s `freeze_dispatch` description, (c) `pve-verify.sh` which fetches the log line via `qm agent exec tail \| grep`. |
| 4 | `guest-get-cpustats` shape | **Fix shape to spec-conformant per-CPU array with `type: "linux"` discriminator.** Use `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` for per-CPU data. `type: "linux"` chosen over `"darwin"` because the upstream `GuestCpuStatsType` enum has no `"darwin"` value (strict parsers would reject it); over omitting `type` because the field is part of the union's required `base` and omitting it is a more severe spec violation. Honest disclosure in code comment + `--self-test-json` env block. |
| 5 | Allowlist alignment | **Keep current 9-command allowlist.** No expansion. Our 9 = upstream's 6 + `guest-sync-id` (extension) + idempotent `freeze` / `freeze-list` (deliberate divergence). Document the principled-restrictive rule; expand only when a concrete consumer needs a specific command. One allowlist regardless of per-FS dispatch type. |
| 6 | Frozen-state persistence | **No change; document divergence.** Our pseudo-freeze isn't a true I/O suspension, so the upstream marker + logging-disable protections aren't needed. Existing APFS snapshot orphan-cleanup already handles the only piece of crash-time state that matters. Windows VSS (same semantic class as us) inherits the same upstream protections without needing them either; we're consistent with that pattern. Documented in `FREEZE_SEMANTICS.md`. |
| 7 | Documentation honesty | **Three revisions:** rewrite `docs/PVE.md` memory-reporting claim; refine `README.md` + `docs/COMPATIBILITY.md` + `src/channel.c` ISA-vs-VirtIO rationale to distinguish VZ from QEMU/OpenCore; create new `docs/design/FREEZE_SEMANTICS.md` with the per-FS dispatch table and divergence notes. |

**Not currently planned (possible future work):** open a qemu-devel discussion proposing a `darwin` variant for `GuestCpuStatsType` and any other macOS-relevant QGA additions. Deferred because (a) FreeBSD and Windows QGAs both *don't register* `guest-get-cpustats` (the spec's `'if': 'CONFIG_LINUX'` gate compiles it out), so there is no upstream precedent for a non-Linux variant; (b) no current consumer needs `type: "darwin"` from us; (c) upstream patch cycles are months. Will surface naturally if the project gets enough attention to justify the effort.

## Implementation queue

Each item below is intended as a focused commit. Ordering: Q1's code change introduces helpers Q2 builds on; Q4 is independent; Q7 lands last so its tables match shipped behaviour.

1. **Q1 + Q3 (agent log) — per-FS dispatch in `sync_all_volumes`.** New `fs_dispatch_class()` helper, per-treatment counter struct, INFO log line summarising the breakdown. Existing single `int` wire response preserved. Unit test in `tests/test_proactive.c` for `fs_dispatch_class` covering each row of the dispatch table.
2. **Q2 — `freeze-list` subset handler.** Distinct `handle_fsfreeze_freeze_list()`, `sync_all_volumes_filtered()` variant taking a mountpoint allowlist. Unit test in `tests/test_proactive.c`.
3. **Q1 (ZFS) — `zfs snapshot` support.** ZFS CLI detection at startup; `create_zfs_snapshot()` / `delete_zfs_snapshot()` mirroring the APFS path; thaw cleanup. Wired into the dispatch table.
4. **Q4 — fix `guest-get-cpustats` shape.** Rewrite `handle_get_cpustats` to return per-CPU array via `host_processor_info(PROCESSOR_CPU_LOAD_INFO)`. Each element: `{type: "linux", cpu: N, user, nice: 0, system, idle}`. Honest in-source comment explaining the `"linux"` choice.
5. **Q5 — allowlist comment + unit test.** No allowlist change. Update the comment on `fsfreeze_command_allowed()` to state the principled-restrictive rule and reference the spec doc. Add a unit test covering representative commands from each category (locks the contract).
6. **Q3 (`--self-test-json`) — `freeze_dispatch` block.** Extend `emit_system_info()` to describe the per-FS dispatch policy, the log path, and the cpustats-discriminator-choice note from Q4.
7. **Q7 — documentation honesty.** All three doc revisions (memory claim, ISA rationale, new `FREEZE_SEMANTICS.md`) in one commit, landing after the code is in.
8. **Phase 3 entry — `pve-verify.sh` enhancements.** Q3's log-fetch wiring, the behavioural-check rewrite (content-not-exit-code per Phase 1 Target 4), the one-shot wrap. Tracked in Phase 3 of `PLAN.md`.
