# Changelog

## Unreleased

### Bug Fixes
- **Fixed:** `guest-get-cpustats` returned a flat aggregate object `{user, system, idle, nice}` summed across vCPUs. The QGA spec defines the response as `['GuestCpuStats']` — an array of per-CPU discriminated-union records with a required `type` field, a `cpu` index, and the per-CPU `user`/`nice`/`system`/`idle` tick counters. Our shape was structurally invalid against the schema at the array level. Rewritten using `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` to produce one entry per vCPU. Each entry tagged `type:"linux"` (the only currently-defined value in upstream `GuestCpuStatsType`; emitting an unknown enum value or omitting the field would be rejected by strict QAPI parsers, see `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q4 for the full reasoning). User/system/idle/nice tick semantics translate cleanly between macOS's `processor_cpu_load_info_t` and Linux's `GuestLinuxCpuStats`. New `tests/run_tests.sh` shape contract verifies array structure, per-entry fields, and `type:"linux"` discriminator. `src/selftest.c`'s safe-test expectation for the command updated from `expect_array=0` to `expect_array=1`.
- **Fixed:** `guest-fsfreeze-freeze-list` silently ignored its optional `mountpoints` argument because it shared the global-freeze handler. A caller asking to freeze only `["/Volumes/data"]` got a global freeze instead of the requested subset. The command now has a distinct handler that parses `args.mountpoints`, validates each entry is a string, and restricts the per-FS dispatch loop to mounts whose `f_mntonname` matches one of the listed paths. With no argument or an empty array it delegates to global freeze (the spec's default). Subset freezes deliberately skip the container-level APFS `tmutil localsnapshot` (snapshotting a whole container for a per-mount request would capture state the caller didn't ask us to capture); per-mount `F_FULLFSYNC` is the consistency mechanism for subset freezes. Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q2. Four integration tests in `tests/run_tests.sh` cover no-args delegation, empty-array delegation, two-mountpoint filter plumbed through (test-mode return matches `n_mountpoints`), and non-string-entry spec-shaped error.
- **Fixed:** Filesystem freeze treated `F_FULLFSYNC` returning `ENOTSUP`/`EOPNOTSUPP` on foreign filesystems (FAT32, exFAT, older MS-DOS drivers, third-party FUSE) as a failure — logging `WARN` and not counting the volume. Per Apple's `fcntl(2)` documentation `F_FULLFSYNC` is only implemented on HFS, MS-DOS (FAT), UDF, and APFS, so the failure is by-design on filesystems that don't implement it; the QEMU Linux QGA reference handles the analogous `EOPNOTSUPP` from `FIFREEZE` by skipping silently. `sync_all_volumes` now dispatches per `f_fstypename`: APFS gets the `tmutil` snapshot + `F_FULLFSYNC`, HFS+ gets `F_FULLFSYNC`, foreign FS tries `F_FULLFSYNC` and counts `ENOTSUP` as `flushed_only` (the global `sync()` at the top already flushed dirty buffers), network mounts (smbfs/afpfs/nfs/webdav) and special FS (devfs/autofs/fdesc/synthfs/volfs/lifs) are skipped categorically. The handler emits a single INFO log line summarising the per-treatment breakdown (snapshotted / zfs_snapshotted / fullfsynced / flushed_only / skipped). The wire response remains a spec-conformant int (sum of "did-something" counters). Reported by @vit9696 in #2; implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q1 + Q3 (log). 21-case unit test for `fs_dispatch_class` in `tests/test_proactive.c`.
- **Fixed:** `guest-get-memory-blocks` fabricated a memory-usage figure when `vm_stat` could not be read. `handle_get_memory_blocks` initialised `used` to `total / 2` and only overwrote it on success, so a `get_vm_stat()` failure silently returned a block list implying exactly 50% RAM used — indistinguishable from a real reading. It now returns a QGA error (`Failed to read memory statistics`) instead of a fabricated value. `guest-get-memory-block-info` (block size) is unaffected; it does not depend on `vm_stat`.

### Internal
- **Refactored:** `fsfreeze_command_allowed` split into a pure-function variant `fsfreeze_is_allowlisted` (checks only the allowlist; ignores `freeze_status`) plus the existing public function that wraps it with the state check. Lets tests exercise the allowlist contract in isolation without needing to manipulate the frozen state. The expanded comment on `fsfreeze_command_allowed` states the principled-restrictive rule from `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q5 (allow during freeze iff handler is read-only, doesn't exec, doesn't change agent state) and notes the deliberate divergences from upstream (`guest-sync-id` extension + idempotent re-freeze). No behaviour change: the same 9 commands are allowed during freeze as before. 30 new unit tests in `tests/test_proactive.c` lock the contract — 9 allowed + 20 representative blocked (writes, exec, suspends, read-only commands deliberately NOT on the list) + NULL guard.

### Tooling
- **Fixed:** `scripts/pve-verify.sh` memory check reported `PASS  memory reporting: 0GB / 0GB`. It read PVE's host-side QMP/balloon counters (blank for macOS guests — macOS ships no virtio-balloon stats driver) by scraping the `pvesh` text table, and printed `PASS` without validating the parsed values. Rewritten: memory now comes from the guest agent itself (`get-memory-block-info` + `get-memory-blocks`), with real used/total derived from its data; agent JSON is parsed with Perl `JSON::PP` instead of `grep`-on-text; the result model is fail-closed, so no check prints `PASS` on data it could not parse; added a `qm`/`perl` preflight and a VM-running check.
- **Fixed:** `scripts/pve-verify.sh` — the `type=isa` config check required `enabled=1` to appear before `type=isa` on the config line, producing a false `FAIL` when Proxmox wrote the agent options in the other order; the two options are now matched independently. The freeze/thaw checks passed on any digit in the output, including a zero-filesystem freeze; they now require a parsed count of at least 1.
- **Added:** `scripts/pve-verify.sh` freeze check now verifies the frozen *state* behaviourally — while frozen, the agent must reject a non-freeze command (`get-osinfo`), and must resume normal operation after thaw — rather than trusting `fsfreeze-status`, which only echoes the agent's internal frozen flag. macOS has no `FIFREEZE`, so the rejection behaviour is the observable proof that the freeze took effect.

### Documentation
- **Updated:** `docs/COMPATIBILITY.md` — promoted **10.4 Tiger** to Tier 1 after @vit9696's v2.4.2 confirmation (issue #2): agent serves PVE end-to-end on 10.4.11 (ping, get-osinfo, network, memory, reboot/shutdown). Matches the convention 15.7 Sequoia already set (Tier 1 with freeze untested).
- **Updated:** `docs/COMPATIBILITY.md` "Step 2: Runtime Validation" sequence now points at `scripts/pve-verify.sh` (one-shot host-side validation with agent-sourced memory + behavioural freeze check) and the modern `--self-test-json` + `--safe-test-json` in-VM diagnostics, replacing the older `tests/safe_test.sh` reference. Added a note on how external contributors submit results (issue comment or PR under `docs/evidence/<version>/`).
- **Fixed:** `docs/CLI.md` Device Auto-Detection section listed the probe order as VirtIO → UTM → ISA. The code in `src/channel.c` has been ISA-first since v2.1.0 — deliberately, because Apple's built-in VirtIO guest agent on Big Sur+ claims the VirtIO channel and ISA is the only one it leaves alone. Reordered the doc to match the code and the v2.1.0 rationale.
- **Added:** `docs/evidence/` directory with a README defining the per-version layout (`selftest.json`, `safetest.json`, `pve-verify.txt`, optional `NOTES.md`) and the submission flow referenced from the reply to issue #2 — so contributors land on a real path with format guidance instead of an empty directory.
- **Added:** `docs/PLAN.md` — phased roadmap (research → configuration matrix and intent design → one-shot validator) covering the deeper freeze/gating/foreign-FS gaps surfaced by @vit9696's Tier-2 submission on 10.4.11. Scaffolded `docs/research/UPSTREAM_NOTES.md` to capture Phase 1 evidence (QGA spec, Linux reference impl, PVE wrapper behaviour, etc.) before any code change.

## v2.4.2 (2026-05-22)

### Bug Fixes
- **Fixed:** Agent never connected on Mac OS X 10.4 Tiger — the serial transport used `poll()`, which returns `POLLNVAL` (0x20) for the serial device on Tiger. macOS `poll()` is implemented on top of kqueue, and Tiger's serial BSD client does not support the kqueue readiness path, so `poll()` reported a valid, open `/dev/cu.serial1` as invalid. The agent treated that as a fatal device error and reconnect-looped every 5 s without ever reading a command — the host saw `QEMU guest agent is not running`. The serial read and write paths in `channel.c` now use `select()`, which uses the legacy `selrecord` path the driver implements and works on every macOS version. Reported by @vit9696 in #2.

### Testing
- **Added:** `tests/test_proactive.c` — channel read over a real PTY, covering the `select()`-based read path (framed-message read and idle timeout). The transport read path previously had no behavioral test coverage.

## v2.4.1 (2026-05-20)

### Bug Fixes
- **Fixed:** `--safe-test` crash on Mac OS X 10.4 Tiger (dyld lazy-bind failure on `_host_statistics64`). Weak-import the symbol so the existing `vm_stat` text fallback in `get_vm_stat()` actually runs on 10.4 instead of the process aborting before the runtime check fires. Reported by @vit9696 in #2.

### Documentation
- **Fixed:** `docs/COMPATIBILITY.md` — `host_statistics64` was incorrectly listed as present on Tiger. It was introduced in 10.6 Snow Leopard. Symbol list and Tiger row corrected; Tiger now noted as relying on the `vm_stat` text fallback for memory stats.
- **Fixed:** `scripts/verify-installer.sh` — `host_statistics64` moved from required to optional in the symbol audit, matching what the binary now actually needs.
- **Added:** Tiger / Leopard PATH note in README — `/usr/local/bin` is not in the default PATH on 10.4–10.5; users should invoke via absolute path or `export PATH=/usr/local/bin:$PATH`.

## v2.4.0 (2026-03-28)

### New Features
- **`--safe-test` / `--safe-test-json`** — built-in read-only command validation. 21 tests, no external script or python needed. Run `sudo mac-guest-agent --safe-test` to verify all read-only commands work correctly.
- **`scripts/pve-verify.sh`** — host-side verification script. Run from PVE host against a VM ID to check config, ping, OS info, network, command count, memory reporting, and freeze round-trip.

### Security Hardening (25 findings addressed)
- **Fixed:** Stop deleting ALL Time Machine snapshots on freeze — now only deletes the snapshot we created
- **Fixed:** Shutdown returns error when fork fails (was silently returning success)
- **Fixed:** SSH key removal returns error when write fails (was silently returning success)
- **Fixed:** Save/restore hibernatemode around suspend (was permanently altered)
- **Fixed:** NULL dereference before null check in channel_create_test
- **Fixed:** Unchecked realloc in SSH key operations (crash on OOM)
- **Fixed:** Memory leak in freeze hook cleanup (empty loop body)
- **Fixed:** Output capture capped at 16MB (matches Linux qemu-ga)
- **Fixed:** tmutil snapshot deletion uses run_command_v (no shell injection)
- **Fixed:** selftest tool_available uses access() instead of system()
- **Fixed:** Signal handler uses volatile sig_atomic_t flag
- **Fixed:** Password zeroed in all code paths with compiler-safe secure_zero
- **Fixed:** setenv() instead of putenv() after fork in guest-exec
- **Fixed:** base64_encode overflow guard for 32-bit
- **Fixed:** json_escape handles control characters
- **Fixed:** Unsupported commands (set-vcpus, set-memory-blocks) registered as disabled
- **Fixed:** LOG_FATAL no longer calls exit() — caller handles cleanup
- **Fixed:** guest-get-diskstats returns structured per-disk stats (was raw text)
- **Fixed:** commands_init guard prevents double-registration

### CI/CD
- Fixed: macos-26 replaced with macos-latest (valid runner)
- Version now single-sourced from Makefile (agent.h uses -DVERSION)

## v2.3.1 (2026-03-28)

### Bug Fixes
- **Fixed:** Malformed JSON input now returns a proper error response per QMP spec instead of being silently discarded. Found by pgcudahy (PR #1).
- **Fixed:** Device detection error message now says "No serial device found" with setup instructions instead of the misleading "No virtio device found."

### Critical Documentation Fix
- **`type=isa` is required on ALL macOS versions.** macOS Big Sur+ ships Apple's own built-in VirtIO guest agent (~18 commands) which claims the default VirtIO serial channel. Using `agent: enabled=1` (default) connects to Apple's agent, not ours — losing freeze, memory reporting, and 27 other commands. ISA serial is the only channel Apple's agent doesn't claim.
- Full comparison of Apple's agent (18 commands) vs ours (45 commands) added to docs/PLATFORMS.md.

### Changes
- ISA serial now checked first in device detection order (was last)
- Run_tests.sh: malformed JSON and missing execute tests un-skipped (65 tests, up from 63)
- PVE.md: "existing VM" troubleshooting for users adding agent to klabsdev-style setups
- LIBVIRT.md: VirtIO channel examples replaced with ISA serial (required)
- COMPATIBILITY.md: Sequoia 15.7.5 promoted to Tier 1 (first external user confirmation)
- COMPATIBILITY.md: PPC status and path to support documented

## v2.3.0 (2026-03-25)

### New Command
- **`guest-network-get-route`** — IPv4 and IPv6 routing table via `netstat -rn`. Achieves 100% Linux qemu-ga command parity (45 commands; only `guest-get-devices` unimplemented, which is Windows-only).

### New Features
- **`--self-test` and `--self-test-json`** — environment diagnostics with backup readiness check. Reports freeze method, kext version, APFS/VirtIO capabilities, hook validation, and overall backup readiness verdict.
- **Backup readiness section** in self-test: freeze method (APFS snapshot / sync / sync-only), root capability, hook count, overall verdict.
- **i386 binary** — cross-compiled via MacOSX10.13.sdk for Tiger (10.4) and Leopard (10.5) support.
- **Baud rate set to 115200** — explicit max baud rate on serial port. QEMU ignores baud rate on virtual serial, but macOS kext may use it for internal pacing.

### Platform Support
- **UTM** — auto-detects `/dev/cu.virtio` (Apple Virtualization.framework)
- **libvirt/virt-manager** — domain XML for ISA serial and VirtIO channels, virsh command examples, quiesced snapshots
- **VirtIO prioritized over ISA serial** on Big Sur+ (native driver preferred when available)
- Device detection order: VirtIO (QEMU/PVE/libvirt) → UTM → ISA serial (fallback)

### Documentation
- **Restructured README** — quick-start focused, detailed content moved to docs/
- **docs/PVE.md** — complete Proxmox VE operational guide with troubleshooting
- **docs/LIBVIRT.md** — full libvirt/virt-manager deployment guide with domain XML examples
- **docs/UTM.md** — UTM guide with utmctl comparison, CI/CD workflows, headless automation
- **docs/BACKUP.md** — freeze mechanics, hook scripts, TRIM guide
- **docs/CLI.md** — all flags, config file, device auto-detection
- **docs/PLATFORMS.md** — platform index with transport priority
- **configs/hooks/** — ready-to-use freeze hooks for MySQL, PostgreSQL, Redis, launchd services
- **configs/pve/** — anchor VM configurations for Tiger, High Sierra, Big Sur, Sequoia

### Compatibility
- **18 macOS versions researched** (10.4 Tiger through 26.3 Tahoe)
- **Apple16X50Serial.kext** verified present on every version with identical PCI class match
- Kext version timeline: v1.6 (Tiger base) → v1.7 (Tiger Intel 10.4.5) → v1.9 (Tiger 10.4.11 combo / Leopard) → v3.0 (Snow Leopard / Lion) → v3.1 (Mountain Lion) → v3.2 (Mavericks through Tahoe)
- **Installer-verified:** 10.4 through 11.6 (12 versions, deep verification: kext + symbols + frameworks + PCI class)
- **Runtime-tested:** 10.11.6 El Capitan (PVE), 26.3 Tahoe (native)

### CI/CD
- **Multi-version test matrix:** macos-14, macos-15, macos-26
- **i386 build** via legacy MacOSX10.13.sdk download in CI
- Self-test validation (text + JSON) in CI pipeline
- ASAN smoke tests expanded to 15 commands
- 48 unit + 31 proactive + 210k fuzz + 63 integration tests

### Fixes
- LaunchDaemon plist: `--daemon` changed to `--daemonize` (primary flag name)
- Command count corrected to 45 across all docs
- Test count corrected to 63 across all docs
- Evidence terminology standardized: runtime-tested, PVE-integrated, installer-verified, best-effort
- All version claims made consistent (10.4+ not 10.7+)

## v2.2.0 (2026-03-23)

### Major Changes
- **Real filesystem freeze** — replaces fake no-op with actual freeze:
  - APFS (10.13+): atomic COW snapshot via `tmutil localsnapshot`
  - All versions: `sync()` + `F_FULLFSYNC` flushes data to physical media
  - Continuous `sync()` every 100ms during freeze window
  - Auto-thaw safety timeout (10 minutes)
  - Command filtering: only freeze-safe commands allowed during freeze
- **Freeze hook scripts** — `/etc/qemu/fsfreeze-hook.d/` (same model as Linux qemu-ga)
  - Scripts called with "freeze"/"thaw" argument
  - 30-second per-script timeout
  - Strict ownership validation (root-owned, not world-writable)

### Security Fixes
- Fixed password memory exposure (zero on all exit paths)
- Fixed command injection in diskutil calls (use execv, not shell)
- Fixed command injection in service update (use execv, not shell)
- Fixed unchecked `pipe()` in guest-exec (could use uninitialized fds)
- Fixed unchecked `fork()` in shutdown handler
- Fixed `WIFSIGNALED` called on extracted exit code instead of raw wait status
- Check all `mkdir()`, `chown()`, `tcsetattr()` return values
- Replace all `strtok()` with thread-safe `strtok_r()`

### Testing
- 48 unit tests + 31 proactive tests + 210,000 fuzz rounds + 62 integration tests
- Code coverage: 55.74% line, 80.27% function (remaining is untestable-in-CI code)
- Proactive tests: channel API, SSH key operations, hook validation, injection prevention

### Documentation
- Backup consistency section in README (freeze behavior, hook scripts, limitations)
- Thin disk provisioning guide (ssd=1, trimforce, TRIM, zero-fill reclaim)
- SECURITY.md updated with freeze hook security model

## v2.1.0 (2026-03-21)

### Major Changes
- **ISA serial transport** — uses Apple's built-in `Apple16X50Serial.kext` instead of VirtIO serial. No custom kernel extensions, no SIP issues, no code signing required. Works on macOS versions with the built-in Apple16X50Serial.kext driver.
- **PVE setup**: `qm set <vmid> --agent enabled=1,type=isa`

### Security
- Password changes via `dscl` now pipe password through stdin instead of command line arguments (no longer visible in `ps aux`)
- Passwords are zeroed in memory after use
- SECURITY.md documenting trust model and hardening options

### Fixes
- Serial port raw mode (no ICANON, no OPOST, no ECHO) for reliable bidirectional communication
- Buffer-check-before-poll: immediately process queued commands when PVE sends sync + command in one write
- Silently discard malformed messages to prevent stale data corruption in the serial buffer
- Removed O_NONBLOCK from serial port open (caused writes to not flush on macOS)

### Features
- `block-rpcs` and `allow-rpcs` fully implemented (were previously parsed but not enforced)
- Log rotation via newsyslog (5 files, 1MB max each)
- ISA serial device auto-detection (`/dev/cu.serial1`)
- Big Sur+ also works with default `type=virtio` via Apple's native VirtIO driver

### Removed
- VirtIO serial kernel extension (unnecessary with ISA serial)

## v2.0.0 (2026-03-21)

### Initial Release
- Native C implementation of the QEMU Guest Agent protocol
- 44 registered QGA commands (34 stable, 5 caveated, 1 no-op, 2 error, 2 aliases)
- Zero external dependencies (cJSON embedded)
- CLI flags compatible with Linux `qemu-ga`
- Configuration file compatible with `/etc/qemu/qemu-ga.conf`
- LaunchDaemon service with `--install` / `--uninstall`
- Binaries: i386 (10.4+), x86_64 (10.6+), arm64 (11.0+), universal
- Tested on macOS Tahoe 26.3 and Mac OS X El Capitan 10.11.6
