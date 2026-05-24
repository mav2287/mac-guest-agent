# Roadmap: freeze rigor, command gating, validator streamline

## Context

This plan addresses what emerged from @vit9696's macOS 10.4 Tier-2 submission (issue #2). Two findings — `F_FULLFSYNC` failing on his FAT32 RECOVERY volume, and `scripts/pve-verify.sh`'s behavioural check reporting "answered while frozen" despite the agent's freeze gating being unconditional in `src/agent.c:73` — point at deeper gaps in our implementation rather than isolated bugs:

- Our freeze handler in `src/cmd-fs.c` assumes HFS+/APFS-only and treats `F_FULLFSYNC` returning `ENOTSUP`/`EOPNOTSUPP` on foreign filesystems (by-design behaviour) as a flat failure. The agent logs `WARN`, undercounts the synced-volume total, and the QGA response (`fsfreeze-freeze` returns count) is misleading.
- We don't know empirically how `qm agent` translates a `{"error":...}` agent response into a shell exit code, what shape PVE's UI gauges expect for CPU/memory, or how the reference Linux QGA solves any of this.
- We have no exhaustive catalog of the configurations the agent will encounter (multi-disk topologies, foreign filesystems, OpenCore variants, snapshot pathways, etc.) so we can't say what the "right" behaviour is per dimension.

Three phases, **strict sequential** — each consumes the previous phase's deliverable. Phase 3 cannot run in parallel with 1 or 2: significant findings in 1 or 2 would change what Phase 3 wraps.

## Phase 1 — Research what's already out there

**Status:** not started

**Goal:** stop guessing about QGA spec details, PVE wrapper behaviour, and reference implementations. Build a citable evidence base before designing anything.

**Research targets** (each becomes a section in `docs/research/UPSTREAM_NOTES.md`):

| # | Source | Questions it answers |
|---|---|---|
| 1 | QEMU `qga/qapi-schema.json` + `docs/interop/qemu-ga-ref.rst` | Canonical schema for `guest-fsfreeze-freeze`, `guest-fsfreeze-status`, `guest-get-cpustats`, `guest-get-memory-blocks`. Settles the "per-CPU array vs aggregate object?" question for `get-cpustats`. |
| 2 | QEMU `qga/commands-posix.c` | Linux freeze reference impl: how does it handle foreign FS, partial failure, what does it return on partial success? |
| 3 | QEMU `qga/main.c` | Linux command-gating state machine. Validates or contradicts our `fsfreeze_command_allowed`. |
| 4 | Proxmox `PVE/QemuServer/Agent.pm` + `PVE/CLI/qm.pm` | What `qm agent <cmd>` and `qm guest cmd <cmd>` do with a `{"error":...}` response. Exit-code semantics. Resolves Finding 2 from the vit9696 thread definitively. |
| 5 | Proxmox UI / RRD source | Whether the CPU% and memory gauges call `guest-get-cpustats` / `guest-get-memory-blocks`, or come from QMP host-side. Closes @vit9696's CPU question on real evidence. |
| 6 | Apple's built-in QGA (Big Sur+, 18 cmds) — binary inspection | How Apple's macOS-native agent shapes its responses, especially for overlapping commands. |
| 7 | virtio-balloon stats protocol | The "right" Linux memory-telemetry path. Confirms whether macOS will ever have real memory telemetry. |

**Deliverable:** `docs/research/UPSTREAM_NOTES.md` — research dossier with quoted code, schema excerpts, source URLs, line numbers, and a short verdict per question. Not a fix proposal — evidence.

**Unblocks:** Phase 2's design decisions.

## Phase 2 — Per-FS strategy, response shapes, allowlist alignment, and documentation honesty

**Status:** ready to start (Phase 1 done 2026-05-23 — see `research/UPSTREAM_NOTES.md` for the evidence base)

**Goal:** translate Phase 1's findings into a per-dimension design spec. For each of the seven decisions Phase 1 surfaced, define the agent's *intent*, the *desired outcome*, the failure modes that *must surface*, and the failure modes that are *by-design and must not look alarming*. Produce a single design spec we implement against.

### Design questions Phase 2 must answer

**1. Per-filesystem-type freeze strategy** (the "do we have enough coverage" question). macOS has no `FIFREEZE`. We get exactly what the platform provides per filesystem type. Build the dispatch matrix and settle exact treatment per `statfs.f_fstypename`:

| `f_fstypename` | Proposed strategy |
|---|---|
| `apfs` | `fs_snapshot_create` via `tmutil localsnapshot` (real atomic consistency point) + `sync()` + `F_FULLFSYNC` as defence in depth |
| `hfs` / `hfs+` | `sync()` + `F_FULLFSYNC` (best-effort flush to media; no kernel-level freeze on macOS) |
| `msdos` / `vfat` / `exfat` | `sync()` only; skip `F_FULLFSYNC` (ENOTSUP is by design, not failure) |
| `ntfs` (Apple's native read-only) | Skip — no writes to flush |
| `cd9660` / `iso9660` | Skip — read-only by definition |
| `smbfs` / `afpfs` / `nfs` | Skip — network mount, not the guest's concern |
| `autofs` | Skip — placeholder, opening can trigger unintended automount |
| `devfs` / `fdesc` / `volfs` / `synthfs` | Skip — special kernel filesystems with no useful flush semantics |
| `zfs` (third-party OpenZFS-on-macOS) | If `zfs` CLI present and volume is an OpenZFS dataset, `zfs snapshot` (real atomic snapshot, same model as APFS); otherwise fall back to `F_FULLFSYNC` |
| anything else | Try `F_FULLFSYNC`; on `ENOTSUP`/`EOPNOTSUPP` treat as "skip, don't count, INFO log" per Linux QGA pattern |

Settle: the exact list, log level per outcome, and how the response message accounts for which volumes got which treatment.

**2. `guest-fsfreeze-freeze-list` mountpoints argument.** The QGA spec defines `freeze-list` with an optional `mountpoints: [str]` argument — freeze only the listed mountpoints, leave the rest writable (typical use: back up a data volume while keeping the OS volume writable for the backup tool's own logs). We currently ignore the argument because we register both `freeze` and `freeze-list` against the same `(void)args` handler — anyone passing mountpoints gets a global freeze. Two options:

- Implement the subset behaviour (parse `args.mountpoints`, only operate on listed mounts).
- Unregister `guest-fsfreeze-freeze-list` (don't claim to support what we don't).

Pick one with reasoning.

**3. Honest per-volume reporting in the freeze response.** Today the response is a single integer (`frozen_volume_count`). With per-FS dispatch from Question 1, that integer is structurally misleading — 1 APFS snapshot, 1 HFS+ F_FULLFSYNC, and 1 FAT32 sync-only are not the same operation. Decide:

- Keep the int (spec-required) and surface the per-FS breakdown in the **log line only**.
- Add a structured per-FS-treatment breakdown to the response as a non-spec extension (and break spec on this command).
- Stick with the int as wire response and report the breakdown only via `--safe-test-json` / `--self-test-json`.

Phase 3 wraps whatever output we produce.

**4. `guest-get-cpustats` shape.** We return a flat aggregate object `{user, system, idle, nice}` for a command the QGA spec defines as `['GuestCpuStats']` — a per-CPU array of typed discriminated-union structs gated on `CONFIG_LINUX`. Three options:

- **Extend the union** with a `darwin` variant, produce per-CPU rows via `HOST_PROCESSOR_INFO` / `PROCESSOR_CPU_LOAD_INFO`, bring the response into spec. Most work, most correct.
- **Stop registering on macOS.** Honest about what we support; nothing breaks because PVE doesn't consume it anyway.
- **Keep current shape, document it as a deliberate macOS extension.** Lowest effort; accept the out-of-spec status.

Pick one.

**5. Command-gating allowlist alignment with upstream.** Upstream's allowlist (6 commands) blocks `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` once frozen — only `thaw` exits the state. Ours (9) allows idempotent re-freeze. Decide: keep our idempotent posture and document the divergence, or align with upstream's stricter behaviour? Also consider adding `guest-get-fsinfo` (read-only, safe during freeze) to allowed.

**6. Frozen-state persistence + logging during freeze.** Upstream writes a persistent on-disk marker so a crashed agent detects prior-frozen state on restart, and disables logging to avoid writing to a frozen volume. We do neither. Mostly harmless because our "freeze" isn't a true I/O suspension, but a real divergence: write the marker (Phase 2 spec + Phase 2 implementation), or document explicitly why we don't?

**7. Documentation honesty.** Three specific revisions Phase 1 surfaced:

- `docs/PVE.md` **"Accurate Memory Reporting Without Balloon Driver"** implies our agent makes PVE's UI memory gauge accurate. **It doesn't.** PVE reads cgroup RSS for macOS guests regardless of whether our agent is installed. Rewrite to describe what we actually provide (direct `qm agent get-memory-blocks`, `pve-verify.sh`-sourced reports) vs. what users may assume.
- `README.md` and `docs/COMPATIBILITY.md` **"ISA-because-Apple-claims-VirtIO"** is right for Apple Virtualization.framework hosts (UTM etc.) but oversimplified for QEMU/OpenCore, where Apple's QGA never launches (no `AppleVirtIOAgentDevice` IOKit property). Refine to distinguish VZ hosts from QEMU/OpenCore hosts.
- **What "freeze" means on macOS** — explicit per-`f_fstypename` table, honest about how it differs from Linux's FIFREEZE. Reference Question 1's matrix.

### Dimensions the matrix work has to cover

For Question 1 specifically, the spec doc must catalogue:

- **Filesystem types** present on macOS, including: HFS+, APFS, FAT32 (`msdos`/`vfat`), exFAT, NTFS (Apple's native R/O + Tuxera/Paragon R/W), ISO9660/cd9660, network (`smbfs`/`afpfs`/`nfs`), `autofs`, FUSE drivers (general), ZFS (third-party OpenZFS), special (`devfs`/`fdesc`/`volfs`/`synthfs`).
- **Multi-volume topologies** known to occur: root + Recovery (@vit9696's case, FAT-formatted), root + Time Machine target mounted live, root + data + cache, root + EFI partition (FAT), root + USB pass-through, network shares mounted at boot.
- **Snapshot pathways available**: APFS native (`tmutil localsnapshot` → `fs_snapshot_create`), Time Machine local snapshots (subset of APFS snapshots), ZFS snapshot, none.
- **macOS version capability matrix**: 10.4 → 26 — `tmutil` present, APFS supported, Mach VM APIs available, `vm_stat` text-format variants, kqueue-tty support (closed in v2.4.2 fix).
- **Bootloader / hypervisor variants** that affect what filesystems and devices appear: OpenCore (QEMU/Proxmox), Clover, UTM (Apple VZ), VMware Fusion, plain QEMU.

### Deliverable

`docs/design/AGENT_BEHAVIOUR_SPEC.md` — the answer to all seven design questions, with the per-FS dispatch matrix from Question 1 as its centrepiece.

Implementation work flows from the spec and is tracked as separate commits: `src/cmd-fs.c` (`sync_all_volumes` per-FS dispatch, freeze response detail), `src/cmd-hardware.c` (cpustats shape if we extend), `fsfreeze_command_allowed` (allowlist alignment), possibly `src/agent.c` (frozen-state persistence), plus doc revisions to `docs/PVE.md`, `README.md`, `docs/COMPATIBILITY.md`, and the new freeze-semantics doc.

**Unblocks:** Phase 3 + a confident, principled reply to @vit9696 that covers both his original findings *and* the deeper coverage question his report surfaced.

## Phase 3 — One-shot `pve-verify.sh` + script bug fix

**Status:** blocked on Phase 2

**Goal:** collapse the Tier-2 → Tier-1 contributor flow from three commands to a single host-side invocation, AND fix the behavioural-check bug Phase 1 Target 4 identified.

**Tasks:**

1. **Fix the behavioural-check bug.** Phase 1 Target 4 proved that `qm agent <cmd>` exits 0 even when the agent returns `{"error":...}`, because PVE's `register_command` dispatcher wraps QGA errors as `{result: {error: {...}}}` and the CLI prints + exits 0. The current `pve-verify.sh` check reading `qm agent ... ; echo $?` is broken regardless of whether the agent gates correctly. Rewrite the check to inspect response *content*:
   - Capture `qm agent <vmid> get-osinfo` output.
   - If the output contains `"error"` with description matching the freeze-rejection message → PASS (agent genuinely rejected).
   - If the output contains `"pretty-name"` (the unambiguous signal of a successful `get-osinfo` response) → FAIL (agent accepted while frozen).
   - Anything else → INFO with the raw response for diagnosis.
2. **One-shot wrap.** Add `qm agent <vmid> exec` invocation of `mac-guest-agent --self-test-json` and `--safe-test-json` to `scripts/pve-verify.sh`; poll `qm agent <vmid> exec-status <pid>` until `exited`, base64-decode `out-data`.
3. **Single structured report** — human-readable text section + JSON appendix the maintainer can paste straight into `docs/evidence/<version>/`. JSON shape reflects whatever Phase 2 picked for `--self-test-json` / `--safe-test-json` (especially cpustats and per-FS freeze breakdown).
4. **PII auto-redaction** (IPs, MAC addresses, VM IDs) gated by `--redact` flag, on by default.
5. **Doc sweep** — update `docs/COMPATIBILITY.md` Step 2 and `docs/evidence/README.md` for the one-command flow. Update any per-FS freeze documentation (created by Phase 2) referenced from the validation flow.
6. **Reply to @vit9696** — once Phase 2 has decisions, send the follow-up that:
   - Confirms his behavioural-check FAIL was a bug in our script, not in our agent.
   - Explains the per-FS freeze treatment we landed on (per Question 1).
   - Confirms the F_FULLFSYNC-on-FAT32 warning he saw will become an INFO and the volume will be counted as best-effort flushed.
   - Acknowledges the broader coverage question his report surfaced.

**Deliverable:** updated `scripts/pve-verify.sh`, doc updates, the follow-up issue reply (drafted for review, posted on user OK), CHANGELOG Unreleased entry.

**Why this is last, not parallel:** if Phase 2 changes the cpustats shape or the freeze response detail, Phase 3 wraps a moving target. The streamline has to consume what Phases 1–2 produce.

## Tracking

| Phase | Status | Deliverable | Started | Completed |
|---|---|---|---|---|
| 1 | **done** | `docs/research/UPSTREAM_NOTES.md` | 2026-05-23 | 2026-05-23 |
| 2 | **done** | `docs/design/AGENT_BEHAVIOUR_SPEC.md` + implementation items 1–7 (per-FS dispatch, freeze-list subset, ZFS, cpustats array, allowlist test, `freeze_dispatch` JSON block, doc honesty) + `docs/design/FREEZE_SEMANTICS.md` | 2026-05-23 | 2026-05-23 |
| 3 | **code + docs done; reply to @vit9696 still pending user OK** | `scripts/pve-verify.sh` Phase 3 rewrite (content-not-exit-code behavioural check, `qm guest exec` driven in-VM `--self-test-json` / `--safe-test-json`, freeze-log fetch, structured report + JSON appendix, PII redaction); `docs/COMPATIBILITY.md` Step 2 + `docs/evidence/README.md` updated for the one-shot flow | 2026-05-23 | 2026-05-23 (code/docs); reply pending |

Update this table as phases progress.
