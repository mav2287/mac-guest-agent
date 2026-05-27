# 10.11.6 evidence — v2.5.0 verifier run

**Hardware:** Apple Xserve3,1 (real metal — not emulated), 2× Intel Xeon W5590 @ 3.33 GHz, 8 GiB RAM.
**OS:** Mac OS X 10.11.6 (build 15G22010).
**Filesystem:** HFS+ on `/dev/disk0s2`, journaled.
**PVE side:** redacted VMID on a single-node PVE host (specific identifiers stripped to match `verify.txt` / `verify.json`).

**Agent:** `mac-guest-agent` v2.5.0 — the **tri-fat universal binary** (i386 + x86_64 + arm64). On this 10.11 host, dyld selects the x86_64 slice at load time; `system_info.selected_arch` in `verify.json` confirms `"x86_64"`. Installed at `/usr/local/bin/mac-guest-agent`, running as the `com.macos.guest-agent` LaunchDaemon.

**Verifier:** `scripts/verify.sh` from branch `universal-upgrade-v2.4.4` (PR #6, pre-tag).

**Result:** 38 passed, 0 failed.

## Why this drop matters

This is the first real-hardware verification of the v2.5.0 universal binary's x86_64 slice — the same slice that v2.4.3's `mac-guest-agent-darwin-amd64` was, but now produced under the LC_UNIXTHREAD entry-point recipe (`-Wl,-ld_classic` + `-mmacosx-version-min=10.6` + legacy 10.13 SDK) that was added to fix issue #4 (@vit9696's `dyld: unknown required load command 0x80000028` crash on 10.6/10.7).

10.11 is not where the v2.4.3 bug manifested (10.11 dyld understands `LC_MAIN`), but a clean 10.11 run confirms:

- The new build recipe produces an x86_64 slice that runs end-to-end on real Intel hardware with all 45 commands operational.
- The universal-binary packaging works: a single download covers Tiger through Tahoe; dyld picks the right slice without per-arch URL juggling.
- Issue #4's actual fix (10.6/10.7 dyld load) still needs runtime confirmation on those specific OS versions — that's vit9696's territory and the next gate before the corresponding COMPATIBILITY.md rows move from Tier 2 to Tier 1.

## What this drop confirms (v2.5.0 universal binary)

- **ISA serial transport on bare-metal Xserve3,1.** `IOSerialFamily` v11 + `Apple16X50Serial` v3.2 + `Apple16X50ACPI` v3.2 loaded; `/dev/cu.serial1` opens cleanly. Identical kext stack and PCI class to the v2.4.3 drop — confirms no regression in transport detection.
- **All 45 registered QGA commands.** `--self-test-json` 20/0/0; `--safe-test-json` 21/21.
- **`selected_arch` field present** (new in v2.5.0). Reports `"x86_64"` here, matching `system_info.arch`. Confirms dyld picked the expected slice from the universal binary.
- **Per-FS freeze dispatch.** Single HFS+ root → `f_fullfsync`; 4 special filesystems (devfs, fdesc, autofs, etc.) categorically skipped. Three consecutive freeze cycles all clean — no state leak between cycles. Matches `docs/design/FREEZE_SEMANTICS.md` exactly.
- **Content-based behavioural freeze check.** While frozen, `get-osinfo` is rejected by content; after thaw, the agent answers normally again. Robust against PVE wrapper variations.
- **`freeze_dispatch` JSON contract.** Binary advertises `apfs: tmutil_snapshot+f_fullfsync`, `cpustats_discriminator: linux`, full per-fstypename table — matches the source-of-truth in `src/cmd-fs.c` and the doc table in `FREEZE_SEMANTICS.md`.
- **Mount-dispatch cross-check.** 1 writable non-network non-special mount expected; freeze reported 1. Match.
- **Schema 2.0 appendix.** `host_environment` populated (sw_vers / hardware / kexts / mounts / launchd / log_file), `freeze_cycles_log` has all 3 cycles with `freeze_log_line` captured per cycle.

## Notable

- Single NIC over the VM bridge. Verifier preferentially reports the IPv4 and redacts it in both `verify.txt` and `verify.json`.
- Memory: agent reports 4 of 16 blocks online → ~2 GiB used / ~8 GiB total. Block-quantised estimate; correlates with the host-side balloon-less view (PVE web UI gauge reads cgroup RSS, doesn't read the agent — see `docs/PVE.md`).
- Universal-binary size on disk: 438 KB (i386 132K + x86_64 140K + arm64 153K, plus fat-header alignment padding). The 10.11 install only references the x86_64 slice at runtime; the other slices are dormant.

## Supersedes

This drop overwrites the prior v2.4.3 evidence in this directory. The v2.4.3 run was the last per-architecture-binary release; v2.5.0 is the first universal-only release, so the artifact under test changed (filename, contents, packaging) and the prior drop is no longer descriptive of what's installed. Same VM, same hardware, same metrics — supersedes cleanly.

## Privacy

Hostnames, VMID, IPv4, IPv6, and MAC addresses are redacted by default. The redaction is applied at the verifier level so contributors can submit `verify.txt` / `verify.json` without manual scrubbing. Contributors writing the per-version `README.md` should follow the same convention so the human-readable narrative doesn't undo the script's redaction.
