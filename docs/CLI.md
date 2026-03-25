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

The agent searches for serial devices in this order (first match wins):

1. **VirtIO serial** (Big Sur+ with native AppleVirtIO.kext)
   - `/dev/cu.org.qemu.guest_agent.0` — PVE default, plain QEMU, libvirt
   - `/dev/cu.virtio-console.0`, `/dev/cu.virtio-serial`, etc.
2. **UTM** (Apple Virtualization.framework)
   - `/dev/cu.virtio`, `/dev/tty.virtio`
3. **ISA serial** (all macOS 10.4+ via Apple16X50Serial.kext)
   - `/dev/cu.serial1`, `/dev/cu.serial2`, `/dev/cu.serial`

Override with `-p /dev/cu.serial1` to force a specific device.

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
