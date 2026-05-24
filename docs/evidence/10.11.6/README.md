# 10.11.6 evidence — v2.4.3 verifier run

**Hardware:** Apple Xserve3,1 (real metal — not emulated), 2× Intel Xeon W5590 @ 3.33 GHz, 8 GiB RAM.
**OS:** Mac OS X 10.11.6 (build 15G22010).
**Filesystem:** HFS+ on `/dev/disk0s2`, journaled.
**PVE side:** `<vmid>` on a single-node PVE host (specific identifiers redacted to match `verify.txt` / `verify.json`).

**Agent:** `mac-guest-agent` v2.4.3 (x86_64 build for 10.6+), installed at `/usr/local/bin/mac-guest-agent`, running as the `com.macos.guest-agent` LaunchDaemon.

**Verifier:** `scripts/verify.sh` @ commit `e28bf6c` (post-El-Cap fixes: dropped unsupported `--output-format json` from `qm guest exec`, parsed `--safe-test-json`'s real `{passes,failures}` keys).

**Result:** 38 passed, 0 failed.

## What this drop confirms

- **ISA serial transport on bare-metal Xserve.** `IOSerialFamily` v11 + `Apple16X50Serial` v3.2 + `Apple16X50ACPI` v3.2 loaded; PCI class match. `/dev/cu.serial1` opens cleanly.
- **All 45 registered QGA commands**. Both `--self-test-json` (20/0/0) and `--safe-test-json` (21/21) green.
- **Per-FS freeze dispatch (audit finding 1)**. The single HFS+ root mount gets `f_fullfsync`; the 4 special filesystems (devfs, fdesc, autofs, etc.) are categorically skipped, exactly matching `docs/design/FREEZE_SEMANTICS.md`. Three consecutive freeze cycles all clean — no state leak between cycles.
- **Content-based behavioural freeze check (audit finding fix)**. The decode-the-error-envelope-by-content path works correctly against real PVE on this host.
- **`freeze_dispatch` JSON contract.** The binary advertises `apfs: tmutil_snapshot+f_fullfsync`, `cpustats_discriminator: linux`, full per-fstypename table — matches the source-of-truth in `src/cmd-fs.c` and the doc table in `FREEZE_SEMANTICS.md`.
- **Mount-dispatch cross-check.** Captured mount table → 1 writable non-network non-special mount expected. Freeze reported 1. Match.
- **Schema 2.0 appendix.** `host_environment` populated (sw_vers / hardware / kexts / mounts / launchd / log_file), `freeze_cycles_log` has all 3 cycles with `freeze_log_line` captured per cycle.

## Notable

- The VM's single NIC carries link-local IPv6, a routable IPv6 GUA, and a routable IPv4. The verifier preferentially reports the IPv4 (and redacts it in `verify.txt` / `verify.json`).
- `network-get-interfaces` showed 1 interface — single-NIC setup over the VM bridge.
- Memory: agent reports 4 of 16 blocks online → ~2 GiB used / ~8 GiB total. Block-quantised estimate; correlates with the host-side balloon-less view (PVE web UI gauge reads cgroup RSS, doesn't read the agent — see `docs/PVE.md`).

## Privacy note

Hostname + VMID redacted here to match the defaults `verify.sh` applies to `verify.txt` and `verify.json`. Contributors writing a per-version `README.md` should follow the same convention so the human-readable description doesn't undo the script's redaction.
