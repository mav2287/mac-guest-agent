# Upstream research: QGA spec, Linux reference impl, PVE wrapper behaviour

**Status:** in progress. Phase 1 of `../PLAN.md`.

This document captures evidence — quoted source, schema excerpts, line numbers, URLs — for questions our implementation needs to answer before we touch code. Each section ends with a **Verdict** that states what the finding means for `mac-guest-agent`.

Nothing in this file is a fix proposal. Decisions flow from here into `../design/FREEZE_AND_GATING.md` in Phase 2.

## Method

For each target:
1. Fetch from a canonical source (upstream git, official docs).
2. Quote the relevant code or schema fragment with file path and line range.
3. Record the URL of the source revision consulted.
4. State what question(s) the finding answers and what is still open.
5. Write a one-paragraph **Verdict** stating the implication for our agent.

Where the answer is "not in this source" or "ambiguous," say so. Don't fill in by guessing.

---

## Target 1 — QGA schema (`qga/qapi-schema.json`)

**Questions:**
- Canonical response shape for `guest-get-cpustats` — per-CPU array of structs, or aggregate object?
- Canonical schema for `guest-fsfreeze-freeze` — what does it return on partial success / foreign-FS error?
- Canonical schema for `guest-fsfreeze-status` — string or struct? Values?
- Canonical schema for `guest-get-memory-blocks` — per-block list, what fields?
- Are there commands documented in the spec that we don't implement, or vice versa?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 2 — Linux freeze reference (`qga/commands-posix.c`)

**Questions:**
- How does Linux QGA enumerate the volumes to freeze?
- What does it do when a volume's freeze ioctl (`FIFREEZE`) returns `ENOTSUP` / `EINVAL` / `EPERM`?
- Does it count partial success, or fail the whole operation?
- What gets returned as `frozen_volume_count` on partial success?
- How is the equivalent of our `F_FULLFSYNC` decision made, if at all?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 3 — Linux command-gating state machine (`qga/main.c`)

**Questions:**
- During freeze, which commands does the reference agent allow?
- Where is the allowed-list defined and how is it consulted?
- What error class / desc does it return for a blocked command?
- Does the reference allow `guest-info` and `guest-ping` during freeze (we do)?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 4 — Proxmox `qm agent` wrapper (`PVE/QemuServer/Agent.pm`, `PVE/CLI/qm.pm`)

**Questions:**
- When QGA returns `{"error": {"class": "...", "desc": "..."}}`, does `qm agent <cmd>` exit non-zero?
- Does `qm guest cmd <cmd>` behave the same way? Different?
- What does each print to stdout vs stderr on agent error?
- Has this behaviour changed between PVE versions (the user's PVE host vs @vit9696's PVE 9.1.9)?
- Definitive answer for why `pve-verify.sh`'s behavioural check passed on El Cap and failed on Tiger: is it the wrapper, or our gating?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 5 — Proxmox UI / RRD CPU+memory gauges

**Questions:**
- Does the PVE web UI's per-VM CPU% gauge call `guest-get-cpustats`?
- Does the memory gauge call `guest-get-memory-blocks` or use balloon stats?
- Does it use QMP host-side (`query-cpus-fast`, `query-balloon`) regardless of guest agent?
- Implication: does our `get-cpustats` shape matter for the UI, or is the UI fed from a different path?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 6 — Apple's built-in QGA (Big Sur+, 18 commands)

**Questions:**
- What commands does Apple's agent implement?
- What response shapes does it produce — especially for any commands we both implement?
- Where does it live on disk? Is it a binary we can `nm` / `otool -L` / extract strings from?
- What does it do for `guest-fsfreeze-freeze` on a Big Sur+ APFS guest (if anything)?
- Is there a relationship between Apple's agent claiming the VirtIO channel and any cross-version freeze behaviour we should know about?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 7 — virtio-balloon stats protocol

**Questions:**
- What stats does the virtio-balloon device expose via QMP?
- What driver-side support is required in the guest to populate them?
- Is there any path to memory telemetry on macOS without writing a virtio-balloon-stats kext?
- Should our agent stop pretending to provide memory usage via `get-memory-blocks` and document the limitation instead?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Synthesis

_(filled when all targets are answered)_

A short consolidated statement of what we now know, what's still unknown, and what specific design questions Phase 2 needs to answer.
