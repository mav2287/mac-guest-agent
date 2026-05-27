# CLI Reference

## Flags

Compatible with the Linux `qemu-ga`:

```
  -d, --daemonize        Run as daemon (log to file)
  -p, --path PATH        Device path [default: auto-detect]
  -l, --logfile PATH     Log file path
  -f, --pidfile PATH     PID file path
  -v, --verbose          Debug logging
  -V, --version          Show version
  -b, --block-rpcs LIST  Comma-separated RPCs to disable
  -a, --allow-rpcs LIST  Comma-separated RPCs to allow (allowlist mode)
  -c, --config PATH      Config file [default: /etc/qemu/qemu-ga.conf]
  -D, --dump-conf        Print effective configuration
  -t, --test             Test mode (stdin/stdout, no QEMU needed)
  -h, --help             Show help
      --install          Install as LaunchDaemon
      --uninstall        Uninstall LaunchDaemon
      --self-test        Check environment and report readiness
      --self-test-json   Same as --self-test but output JSON
      --update PATH      Update binary from local file
```

## Configuration File

Optional. Compatible with Linux `/etc/qemu/qemu-ga.conf`:

```ini
[general]
daemonize = 0
path = /dev/cu.serial1
logfile = /var/log/mac-guest-agent.log
verbose = 0
# block-rpcs = guest-exec,guest-set-user-password
# allow-rpcs = guest-ping,guest-info,guest-get-osinfo
```

CLI flags override config file values.

## Device Auto-Detection

As of v2.5.0 the agent is **ISA serial only**. The auto-detect list (first match wins):

- `/dev/cu.serial1`, `/dev/tty.serial1`
- `/dev/cu.serial2`, `/dev/tty.serial2`
- `/dev/cu.serial`,  `/dev/tty.serial`

All of these are backed by `Apple16X50Serial.kext`, which has shipped on every macOS from 10.4 Tiger onwards with an identical PCI class match.

If a VirtIO device is present but no ISA device is, the agent logs a diagnostic identifying the VirtIO path and pointing at the hypervisor reconfiguration steps (`type=isa` on PVE, isa-serial device on libvirt, QemuGuestAgent interface on UTM, `-device isa-serial` on raw QEMU), then exits. See [CHANGELOG v2.5.0 BREAKING — ISA serial transport only](../CHANGELOG.md) for the v2.4.x migration.

Override with `-p /dev/cu.serial1` (or any path you trust) to skip auto-detect entirely.

## Test Mode

```bash
# Interactive — type JSON commands, see responses
mac-guest-agent -t -v

# Pipe a command
echo '{"execute":"guest-ping"}' | mac-guest-agent --test

# Run the full integration test suite
./tests/run_tests.sh ./build/mac-guest-agent
```

Test mode uses stdin/stdout instead of a serial device. Freeze operations run in dry-run mode (no real filesystem changes). Root not required.

## File Locations

| File | Path |
|---|---|
| Binary | `/usr/local/bin/mac-guest-agent` |
| LaunchDaemon | `/Library/LaunchDaemons/com.macos.guest-agent.plist` |
| Config (optional) | `/etc/qemu/qemu-ga.conf` |
| Freeze hooks | `/etc/qemu/fsfreeze-hook.d/` |
| Log | `/var/log/mac-guest-agent.log` |
| Log rotation | `/etc/newsyslog.d/mac-guest-agent.conf` |
