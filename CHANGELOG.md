# Changelog

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
