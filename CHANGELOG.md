# Changelog

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
