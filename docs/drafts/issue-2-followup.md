# DRAFT — issue #2 reply (built from user's seed text)

**Status:** draft for review. Not posted. Intended for
https://github.com/mav2287/mac-guest-agent/issues/2 once approved.

**Built from this seed text the user wrote:** "There is no freeze in macOS
and that was a side effect of the ways the agent works around that. I
also went down a bit of a development and optimization rabbit hole and
changed up the verification testing."

**Context for the reply:** vit9696's most recent issue activity covered
two real findings against v2.4.2: (a) a freeze `[WARN] F_FULLFSYNC
failed on /Volumes/RECOVERY: Operation not supported` on the FAT32
recovery volume he attached to his 10.4 VM (10.4 has no Apple Recovery
support); (b) a `pve-verify.sh` FAIL on `frozen state — agent answered
get-osinfo while frozen (not genuinely frozen)`. PR #3 (now merged + a
follow-up at `b49bbe5`) ships the 10.4.11 evidence. The reply below
closes both threads.

**Tone:** peer-to-peer. No "we believe / claim / you'd know better".
Mirror his "rabbit hole" voice — that's authentic and accurate.

---

There is no freeze on macOS — no `FIFREEZE` like Linux, no `UFSSUSPEND`
like FreeBSD, no public kernel-level write-suspension on any version
from 10.4 through 26. What we call "freeze" is the best
consistency primitive each filesystem actually exposes:

- APFS (10.13+) → atomic container-level COW snapshot via
  `tmutil localsnapshot` + per-mount `F_FULLFSYNC`.
- HFS+ → per-mount `F_FULLFSYNC`. Disk-level flush, no snapshot.
- ZFS (third-party OpenZFS on macOS, when the CLI is present) →
  `zfs snapshot`.
- FAT/exFAT/UDF/NTFS → try `F_FULLFSYNC`; if the kernel returns
  `ENOTSUP`/`EOPNOTSUPP`, fall back to the global `sync(2)` we
  ran at the top of the freeze and continue without a WARN. Apple's
  `fcntl(2)` man page lists which filesystems support the fcntl;
  yours falling out depends on the in-tree driver version on your
  guest.
- Network mounts (smbfs / afpfs / nfs / webdav / ftp) and special
  mounts (devfs / autofs / fdesc / volfs / synthfs / lifs) are
  categorically skipped.

The full dispatch table lives at
[`docs/design/FREEZE_SEMANTICS.md`](https://github.com/mav2287/mac-guest-agent/blob/main/docs/design/FREEZE_SEMANTICS.md)
and the binary advertises the same table statically under
`mac-guest-agent --self-test-json` → `freeze_dispatch` so the policy
is auditable without running a real freeze.

So your `/Volumes/RECOVERY` (FAT32) WARN — that was the agent
treating an `ENOTSUP` from `F_FULLFSYNC` on a foreign-FS-style mount
as a real failure. It was always going to be `ENOTSUP` on that
filesystem; treating it as a WARN was a quality bug. v2.4.3 logs it
at DEBUG and counts the volume as `flushed_only` (the global sync
covered the data), no WARN, freeze continues. Upstream Linux QGA
handles the analogous `EOPNOTSUPP` from `FIFREEZE` the same way.

The `pve-verify.sh` FAIL on `frozen state — agent answered
get-osinfo while frozen` was a script bug, not an agent bug. The
old script ran `qm agent <vmid> get-osinfo` and inspected its exit
code to decide whether the agent had genuinely rejected the
command. PVE's `register_command` dispatcher wraps QGA error
envelopes as `{result:{error:{...}}}` and the CLI exits 0 either
way, so the exit-code check could never distinguish honest
rejection from a silently-served reply. There's a
`FIXME: remove with PVE 8.0` comment in `PVE/CLI/qm.pm` flagging
the missing `check_agent_error` call on that dispatch path —
source walk is in
[`docs/research/UPSTREAM_NOTES.md` Target 4](https://github.com/mav2287/mac-guest-agent/blob/main/docs/research/UPSTREAM_NOTES.md)
if you want to read along.

Your agent on the 10.4 VM was correctly rejecting `get-osinfo`
during the freeze window per `src/agent.c:73`. The script was
lying about what it saw.

The rabbit hole that became v2.4.3 came out of those two findings.
The shape of it:

1. **Research pass** — `UPSTREAM_NOTES.md`. Walked the QGA spec
   (qapi-schema.json), the Linux QGA reference, the Windows QGA
   reference, the PVE register-command dispatcher (the source of
   the exit-code bug), Apple's `AppleQEMUGuestAgent` (universal
   Mach-O on macOS 26.5 — VZ-only, 18 commands, no freeze), and
   `vmstatus.pm` (so we'd stop pretending PVE's web UI memory
   gauge reads our agent — it doesn't, it reads cgroup RSS).
2. **Design pass** — seven design questions answered in
   `AGENT_BEHAVIOUR_SPEC.md`. The per-FS dispatch above came out
   of Q1. `freeze-list` mountpoint filtering (was silently a
   global freeze) came out of Q2. CPU stats schema fix
   (`type:"linux"` per-CPU array — upstream `GuestCpuStatsType`
   has no `"darwin"` enum value) came out of Q4. Freeze-time
   command allowlist came out of Q5.
3. **Implementation** — the per-FS dispatch and freeze-list
   filtering as code; the cpustats fix; the foreign-FS-`ENOTSUP`-
   tolerance; ZFS snapshot support when the CLI is installed;
   and a separate third-party audit pass that surfaced a few
   more real bugs (async `guest-exec` to fix a pipe-drain
   deadlock + the agent-blocking-for-child-lifetime spec
   violation; strict base64 validation; SSH `authorized_keys`
   write hardened against symlink TOCTOU; QGA spec-shaped
   `guest-get-load` / `guest-network-get-route` / `guest-get-
   diskstats` responses; `guest-get-diskstats` now sources real
   cumulative counters from IOKit `IOBlockStorageDriver` instead
   of iostat rate snapshots).
4. **Unified verifier** — `pve-verify.sh` is gone, replaced by
   `scripts/verify.sh`. Auto-detects PVE / libvirt / UTM / raw
   QGA socket; one host-side command drives both host-side
   checks AND the in-VM `--self-test-json` / `--safe-test-json`
   diagnostics via `qm guest exec` (or libvirt / utmctl /
   socket equivalent). Captures a richer schema-2.0 JSON
   appendix with `host_environment` (sw_vers / hardware /
   kexts / mounts / launchd / log_file), `freeze_cycles_log`
   (multi-cycle freeze with the per-event log line per
   cycle), and `mount_dispatch_crosscheck` (the captured
   mount table compared to the actual freeze count, to catch
   dispatch drift). Behavioural check now inspects response
   content, not exit code.

First real-world v2.4.3 evidence is in
[`docs/evidence/10.11.6/`](https://github.com/mav2287/mac-guest-agent/tree/main/docs/evidence/10.11.6)
— Xserve3,1 bare metal, 38 PASS / 0 FAIL, three freeze cycles
all clean.

If you have cycles for a re-run on your 10.4.11 setup with v2.4.3
+ `scripts/verify.sh`, the FAT32 WARN will be gone and the
behavioural check will PASS by content — that'd close out 10.4
definitively. Genuinely optional though; your existing data is
what made v2.4.3 happen and it's already preserved in
[`docs/evidence/10.4.11/`](https://github.com/mav2287/mac-guest-agent/tree/main/docs/evidence/10.4.11)
via your PR.
