# 10.11.6 evidence — v2.5.3 verifier run

**Hardware:** Apple Xserve3,1 (real metal — not emulated), 2× Intel Xeon W5590 @ 3.33 GHz, 8 GiB RAM.
**OS:** Mac OS X 10.11.6 (build 15G22010).
**Filesystem:** HFS+ on `/dev/disk0s2`, journaled.
**PVE side:** redacted VMID on a single-node PVE host (specific identifiers stripped to match `verify.txt` / `verify.json`).

**Agent:** `mac-guest-agent` v2.5.3 — the tri-fat universal binary (i386 + x86_64 + arm64). On this 10.11 host, dyld selects the x86_64 slice at load time; `system_info.selected_arch` in `verify.json` confirms `"x86_64"`. Installed at `/usr/local/bin/mac-guest-agent`, running as the `com.macos.guest-agent` LaunchDaemon (PID 42136 in this run, `LastExitStatus = 0`).

**Verifier:** `scripts/verify.sh` from `main` (script_version `2026-05-23-verify-v2`).

**Result:** 38 passed, 0 failed.

## What this drop confirms (fourth successful v2.5.x run on the same hardware)

This evidence supersedes the prior v2.5.2 drop in this directory. Four consecutive v2.5.x releases (v2.5.0 → v2.5.1 → v2.5.2 → v2.5.3) have now produced byte-structurally-identical evidence on the same Xserve3,1 / 10.11.6 stack — `selected_arch=x86_64`, 38/0 counts, identical kext stack (`IOSerialFamily v11 + Apple16X50Serial v3.2 + Apple16X50ACPI v3.2`), identical freeze-dispatch contract, identical mount-dispatch cross-check. The only run-to-run drift is the expected kind: timestamps, launchd PID, log file size.

v2.5.3-specific things this drop confirms in addition to the v2.5.0 baseline:

- **The install state machine refactor doesn't regress standard-install behavior.** v2.5.3 moved `--virtio` / `--upgrade` / detection logic out of `scripts/install.sh` and into `src/service.c`. For an ISA-serial install (this drop's case), the operator-visible behavior and the host-side verify output are byte-equivalent to v2.5.2 across all 38 checks. The refactor is invisible to the supported configuration.
- **The new `--upgrade` flow works on real hardware.** This drop's binary was installed by running `sudo /private/tmp/mac-guest-agent --upgrade` (the new self-source flow — no PATH arg). Backup-and-restore-on-failure code path exercised; restored binary path tested via clean uninstall + reinstall cycles before the verify run captured here.
- **Self-source `--install` works.** `sudo /private/tmp/mac-guest-agent --install` copies the binary from `/private/tmp/` to `/usr/local/bin/` automatically — no manual `mv` step. The verify run was performed against the install produced this way.
- **Marker-aware `--uninstall` works.** Tested via a `--install --virtio-force` (which dropped the marker as `mode=force`), then `--uninstall` (which correctly removed `/etc/qemu/qemu-ga.conf` and left Apple's daemon alone since `mode=force` means we never unloaded it).
- **All 14 audit findings from the v2.5.3 cycle remain closed** — see CHANGELOG v2.5.3.

## What this drop confirms (vs the v2.5.0 baseline)

- **ISA serial transport on bare-metal Xserve3,1.** `IOSerialFamily` v11 + `Apple16X50Serial` v3.2 + `Apple16X50ACPI` v3.2 loaded; `/dev/cu.serial1` opens cleanly. Identical kext stack across all v2.5.x runs — no regression in transport detection.
- **All 45 registered QGA commands.** `--self-test-json` 20/0/0; `--safe-test-json` 21/21.
- **`selected_arch` field present.** Reports `"x86_64"`, matching `system_info.arch`. Confirms dyld picked the rebuilt-for-issue-#4 slice from the universal binary.
- **Per-FS freeze dispatch.** Single HFS+ root → `f_fullfsync`; 4 special filesystems (devfs, fdesc, autofs, etc.) categorically skipped. Three consecutive freeze cycles all clean — no state leak between cycles. Matches `docs/design/FREEZE_SEMANTICS.md` exactly.
- **Content-based behavioural freeze check.** While frozen, `get-osinfo` is rejected by content; after thaw, the agent answers normally again.
- **`freeze_dispatch` JSON contract.** Binary advertises `apfs: tmutil_snapshot+f_fullfsync`, `cpustats_discriminator: linux`, full per-fstypename table — matches the source-of-truth in `src/cmd-fs.c`.
- **Mount-dispatch cross-check.** 1 writable non-network non-special mount expected; freeze reported 1. Match.
- **Schema 2.0 appendix.** `host_environment` populated (sw_vers / hardware / kexts / mounts / launchd / log_file), `freeze_cycles_log` has all 3 cycles with `freeze_log_line` captured per cycle.

## Notable

- Single NIC over the VM bridge. Verifier preferentially reports the IPv4 and redacts it in both `verify.txt` and `verify.json`.
- Memory: agent reports 4 of 16 blocks online → ~2 GiB used / ~8 GiB total. Block-quantised estimate; correlates with the host-side balloon-less view (PVE web UI gauge reads cgroup RSS, doesn't read the agent — see `docs/PVE.md`).
- Universal-binary size on disk: ~450 KB (i386 + x86_64 + arm64 slices plus fat-header alignment padding). The 10.11 install only references the x86_64 slice at runtime; the other slices are dormant.

## Supersedes

This drop overwrites the prior v2.5.2 evidence in this directory. v2.5.0 was the first universal-only release with the issue #4 LC_UNIXTHREAD fix; v2.5.1 renamed the asset to `mac-guest-agent` at @vit9696's request; v2.5.2 closed audit Finding 2's `guest-exec` slot leak; v2.5.3 moved the install state machine into the binary (install.sh is now a thin bootstrap wrapper) and closes 14 audit findings across two cycles. Same VM, same hardware, same metrics across all four releases.

## Privacy

Hostnames, VMID, IPv4, IPv6, and MAC addresses are redacted by default. The redaction is applied at the verifier level so contributors can submit `verify.txt` / `verify.json` without manual scrubbing. Contributors writing the per-version `README.md` should follow the same convention so the human-readable narrative doesn't undo the script's redaction.
