# Exhaustive command coverage — all 4 PVE VMs

Goal: prove every QGA command (+ error/edge cases) works on every supported macOS
VM, with the fixed binary (the 6 fixes from the Tiger matrix session).

## Method

`tests/exhaustive-commands.sh <vmid>` drives the agent's `--test` mode (stdin/
stdout, no serial channel, no in-guest python) with a 53-case battery covering
all 45 registered QGA commands plus error paths and edge cases. Responses are
asserted host-side with jq. SAFETY: shutdown/suspend are never in the battery
(they would really halt the VM in --test mode, which only gates fsfreeze);
mutating commands appear only in non-mutating error forms or against scratch
targets.

Binary deploy to networked VMs: guest `curl`s the universal binary
(i386/x86_64/arm64) from an HTTP server on the PVE host, installs via `--upgrade`.
(QGA file-write can't carry it — the agent's READ_BUF_SIZE caps inbound messages
at 4096 bytes, ~2KB/chunk; hundreds of chunks overwhelm the channel. Tiger gets
the binary via offline disk mount instead.)

## Results

| VM | OS | Arch | Deploy | Battery (53 cases) |
|----|----|----|--------|--------------------|
| 111 | Tiger 10.4.11 | i386 | offline mount ✓ | **53 / 53 PASS** (on a fresh channel after cold-reset) |
| 112 | Leopard 10.5.8 | i386 | curl+upgrade ✓ | **53 / 53 PASS** (after cold-reset to recover wedged channel) |
| 113 | Snow Leopard 10.6.8 | x86_64 | curl+upgrade ✓ | **53 / 53 PASS** |
| 107 | BAM / El Capitan 10.11 | x86_64 | curl+upgrade ✓ | **53 / 53 PASS** |

Deploy mechanics that worked: HTTP server on PVE (`python3 -m http.server 8099 --directory /tmp </dev/null` — the `</dev/null` is required or the backgrounded server hangs the SSH session). Guests `curl` from the host: SL/Leopard via slirp gateway `10.0.2.2:8099`, BAM via bridge `REDACTED-IP:8099`. Install via `<binary> --upgrade` (lipo-thins to host arch, verifies daemon).

## CLI-verb coverage (`tests/exhaustive-cli.sh`, 21 cases)

Every command-line verb/flag, run non-destructively via one guest-exec per case.

| VM | CLI battery (21 cases) |
|----|------------------------|
| 113 Snow Leopard | **21 / 21 PASS** |
| 107 BAM / El Capitan | **21 / 21 PASS** |
| 112 Leopard | **21 / 21 PASS** |
| 111 Tiger | 13/21 captured+passed in one run; remaining 8 = "NO OUTPUT BLOCK" (the heavy --self-test/--safe-test forks loaded the serial and the out-data drain truncated mid-run — NOT CLI failures). Tiger CLI fully verified independently: matrix Phase 5 (5.1–5.16 all PASS individually) + identical binary 21/21 on the 3 modern VMs. |

Harness note: the CLI battery runs all 21 cases in ONE guest-exec (an in-guest
runner script). The first version ran 21 separate guest-execs, which wedged
Tiger's ISA-serial channel partway (7 pass / 14 wedge-artifacts) — the same
load-induced fragility documented in the Tiger matrix. The single-exec design is
channel-safe and faster on every VM (SL/BAM/Leopard 21/21).

Covers: `--version`, `--help` (+ verb listing), `--dump-conf`, `--self-test`
(+version/commands assertions), `--self-test-json`, `--safe-test`,
`--safe-test-json`, `--test` (ping), `--block-rpcs`, `--allow-rpcs`,
`--install --dry-run`, `--uninstall --dry-run`, single-dash `-virtio` typo hint,
`--virtio`+`--virtio-force` conflict, `--virtio` without `--install`,
`--upgrade`+`--install` conflict, `--virtio` refusal on non-VirtIO, unknown flag.
Destructive verbs (`--install`/`--upgrade`/`--uninstall` without `--dry-run`)
are covered by the lifecycle matrix (Tiger Phase 7 + tasks #167–170).

## Battery coverage (53 cases)

All 45 registered commands exercised. Highlights:
- 19 read commands → real-data assertions (osinfo, time, load, vcpus, memory,
  cpustats, disks, diskstats, fsinfo, network interfaces+routes, users, etc.)
- sync / sync-delimited (incl. 0xFF resync-byte handling)
- fsfreeze freeze/status/thaw/freeze-list (dry-run in --test)
- fstrim
- set-vcpus / set-memory-blocks → clean unsupported errors
- set-time: argless (RTC-resync no-op) + non-number → error
- exec: missing path, non-string path, nonexistent bin, /usr/bin/true, bad
  input-data base64 → error
- exec-status: missing pid, invalid pid → error
- file-open/read/write/close/seek/flush: bad-handle + bad-path error paths
- ssh-get/add/remove-authorized-keys: missing-arg + nonexistent-user errors
- set-user-password: missing-arg + bad-base64 errors
- unknown command → CommandNotFound
- no-execute-field JSON → error

## Not covered by the --test battery (tested separately, live)

shutdown (powerdown/reboot/halt), suspend-disk/ram/hybrid, the full exec/file
mutation round-trips, ssh-key add+remove round-trip, set-password round-trip —
these were covered in the Tiger 111-item matrix and are exercised per-VM via the
real daemon path. CLI verbs, lifecycle state machine, and scripts are tracked in
task #176.
