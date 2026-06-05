# v2.5.4 validation evidence

Empirical evidence collected during the v2.5.4 development cycle that
addresses upstream issues #9 (x86_64 slice fails to load on Tiger
10.4.7+) and #10 (agent stops responding after extended uptime on Tiger).

Methodology: same agent binary (a tri-fat universal `mac-guest-agent` with
i386 + x86_64 + arm64 slices) deployed identically to each environment.
For Tiger targets we compare the **shipped v2.5.3 release artifact**
(SHA-256 `7f054024c8f31319a3e22d49680f6e08fc4c893aad2cbff1cd08c1dddd1cca0d`,
downloaded via `gh -R mav2287/mac-guest-agent release download v2.5.3`)
against the v2.5.4 candidate build.

## Validation matrix

| Target | OS | Slice selected by XNU | v2.5.3 (shipped) | v2.5.4 (candidate) |
|---|---|---|---|---|
| iMac5,1 (Core 2 Duo, real hardware) | 10.4.5 / Darwin 8.5.3 | i386 (no x86_64 userland in 10.4.5) | ✅ runs | ✅ runs |
| QEMU/KVM (Proxmox VE 9, pc-q35-6.1, Penryn) | 10.4.10 / Darwin 8.10.3 | **x86_64** (bug-trigger path) | ❌ `dyld: unknown required load command 0x80000022` | ✅ runs |
| QEMU/KVM (Proxmox VE 9, pc-q35-6.1, Penryn) | **10.4.11 / Darwin 8.11.1** (vit9696's reported version) | **x86_64** (bug-trigger path) | n/a (not reshipped) | ✅ runs, full QGA protocol works |

## Files in this directory

- [`eof-storm-fix-validation.txt`](eof-storm-fix-validation.txt)
  — **The decisive piece for issue #10**: 6 h 54 m of continuous
  uptime on Tiger 10.4.11 processing 10,288 messages with zero wedges,
  zero EIO reconnects, and zero watchdog firings after deploying the
  EOF-storm root-cause fix. Includes the full counter dump timeline
  for postmortem analysis and a comparison against the pre-fix wedge
  cadence (4-6 minutes between wedges in the same environment).
- [`tiger-10.4.11-vm-x86_64-pass.txt`](tiger-10.4.11-vm-x86_64-pass.txt)
  — Full `--version` + `--self-test-json` + `DYLD_PRINT_LIBRARIES` output
  from v2.5.4 running on the Tiger 10.4.11 VM via the x86_64 slice. The
  decisive piece for issue #9: the binary loads cleanly where v2.5.3
  panicked dyld, and self-test passes 12/12 with `"selected_arch":"x86_64"`.
- [`tiger-10.4.5-real-i386-pass.txt`](tiger-10.4.5-real-i386-pass.txt)
  — Same on real iMac5,1 hardware via the i386 slice (the only one 10.4.5
  exposes). Bug doesn't trigger here because pre-10.4.7 Tiger has no
  x86_64 userland, but proves the i386 slice is regression-free.
- [`tiger-10.4.10-vm-x86_64-v253-fail.txt`](tiger-10.4.10-vm-x86_64-v253-fail.txt)
  — vit9696's reported failure, reproduced bit-for-bit on the Tiger
  10.4.10 VM with the shipped v2.5.3 binary. The smoking gun: `dyld:
  unknown required load command 0x80000022` (LC_DYLD_INFO_ONLY).
- [`pve-qm-agent-tiger-10.4.11.txt`](pve-qm-agent-tiger-10.4.11.txt)
  — PVE-host-side `qm agent 111 ping` / `qm agent 111 get-osinfo` output
  showing the daemon is fully QGA-protocol-compliant on Tiger 10.4.11
  with the v2.5.4 lipo-thin install in place.
- [`channel-status-dump.txt`](channel-status-dump.txt) — the SIGUSR1
  channel-status diagnostic dump from the v2.5.4 daemon on Tiger
  10.4.11. New diagnostic tool added in v2.5.4 per Codex review for
  future issue-#10 reproductions.
- [`lipo-thin-install-tiger-10.4.11.txt`](lipo-thin-install-tiger-10.4.11.txt)
  — Demonstrates the lipo-thin install path (vit9696's suggested
  improvement on issue #9): source fat binary 498 KB, installed
  single-arch binary 158 KB, ~340 KB saved.

## Reproduction notes

- The Tiger 10.4.10 / 10.4.11 VMs run on Proxmox VE 9 with the config
  documented in [`docs/TIGER_ON_PVE.md`](../../TIGER_ON_PVE.md).
- Networking via `-netdev user` (slirp NAT) with `hostfwd=tcp:0.0.0.0:22111-10.0.2.15:22`
  for SSH — required because tap/bridge networking is broken for Tiger
  10.4 under modern QEMU's e1000 PHY auto-neg simulation (separate from
  issues #9/#10, documented in the Tiger guide).
- `agent: enabled=1,type=isa` plus a writethrough cache disk config —
  the writethrough is mandatory if you want HFS+ journal to survive
  host-side resets without corruption (also documented in the guide).
