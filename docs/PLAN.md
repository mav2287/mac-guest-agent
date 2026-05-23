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

## Phase 2 — Configuration matrix and intent design

**Status:** blocked on Phase 1

**Goal:** anticipate the real configurations our agent will encounter; for each dimension define the agent's *intent* and *desired outcome*. Produce a design spec we implement against.

**Dimensions to catalog** (each becomes a row/column in a matrix):

1. **Filesystem types**: HFS+, APFS, FAT32, exFAT, msdos, ext*, NTFS, ISO9660, autofs, network mounts (smbfs, nfs, afpfs), read-only mounts, mounted DMG snapshots, FUSE volumes. Per type: `F_FULLFSYNC` support, `sync()` sufficiency, freeze semantics, what "frozen" should mean.
2. **Disk hardware**: VirtIO, IDE/SATA, AHCI, SCSI, NVMe pass-through.
3. **Snapshot pathways**: native APFS (`tmutil localsnapshot`), Time Machine local, PVE zvol/qcow2, layered.
4. **Multi-volume topology**: root + recovery (@vit9696's case), root + data, root + cache, Time Machine target mounted live.
5. **Bootloader / hypervisor variants**: OpenCore, Clover, raw PVE OVMF, UTM (Apple Virtualization.framework), VMware Fusion, plain QEMU.
6. **macOS versions**: 10.4 → 26 capability matrix — `F_FULLFSYNC`, `tmutil`, APFS, Mach VM APIs, kqueue-tty support, `vm_stat` text-format drift.
7. **Command gating** across all 45 QGA commands: freeze-safe / freeze-unsafe / depends — derived by analysis, not intuition. Current allowed list (9 commands in `fsfreeze_command_allowed`) was set heuristically.

For each cell: (a) the agent's intent for the relevant commands, (b) the desired outcome, (c) failure modes that must surface, (d) failure modes that are by-design and must not look alarming.

**Deliverable:** `docs/design/FREEZE_AND_GATING.md` — spec doc per command-family with the configuration matrix, intent statements, failure-mode tables. Implementation work (changes to `src/cmd-fs.c`, `src/cmd-hardware.c`, `fsfreeze_command_allowed`, possibly `src/agent.c`) flows from it but is tracked as separate commits.

**Unblocks:** Phase 3 + a confident, principled reply to @vit9696.

## Phase 3 — One-shot `pve-verify.sh`

**Status:** blocked on Phase 2

**Goal:** collapse the Tier-2 → Tier-1 contributor flow from three commands (two in-VM, one host-side) to a single host-side invocation.

**Tasks:**

1. Add `qm agent <vmid> exec` invocation of `mac-guest-agent --self-test-json` and `--safe-test-json` to `scripts/pve-verify.sh`; poll `qm agent <vmid> exec-status <pid>` until `exited`, base64-decode `out-data`.
2. Emit a single structured report (human-readable text section + JSON appendix the maintainer can paste straight into `docs/evidence/<version>/`).
3. PII auto-redaction (IPs, MAC addresses, VM IDs) gated by `--redact` flag, on by default.
4. Update `docs/COMPATIBILITY.md` Step 2 and `docs/evidence/README.md` for the one-command flow.
5. Update the reply pattern referenced in the existing issue-reply template (if any).

**Deliverable:** updated `scripts/pve-verify.sh`, doc updates, CHANGELOG Unreleased entry.

**Why this is last, not parallel:** if Phase 1 finds we should change the QGA invocation wrapper (e.g. `qm guest exec` instead of `qm agent exec`), or Phase 2 changes the shape of `--self-test-json` / `--safe-test-json` output, Phase 3 wraps a moving target. The streamline has to consume what Phases 1–2 produce.

## Tracking

| Phase | Status | Deliverable | Started | Completed |
|---|---|---|---|---|
| 1 | **done** | `docs/research/UPSTREAM_NOTES.md` | 2026-05-23 | 2026-05-23 |
| 2 | ready to start | `docs/design/FREEZE_AND_GATING.md` | — | — |
| 3 | blocked on Phase 2 | `scripts/pve-verify.sh` updates | — | — |

Update this table as phases progress.
