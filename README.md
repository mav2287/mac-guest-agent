# macOS QEMU Guest Agent

A native QEMU Guest Agent for macOS, written in C. Enables hypervisors (Proxmox VE, libvirt, plain QEMU) to manage macOS virtual machines through the standard QGA protocol.

**Supports Mac OS X 10.4 Tiger through macOS Tahoe and beyond.**

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/install.sh | sudo bash
```

Or manually:

```bash
# Download (Intel Mac)
curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-amd64
# Download (Apple Silicon)
curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-arm64

chmod +x mac-guest-agent
sudo cp mac-guest-agent /usr/local/bin/
sudo mac-guest-agent --install
```

## Service Management

Mirrors the Linux `systemctl` workflow using macOS `launchctl`:

| Linux (systemd) | macOS (launchctl) |
|---|---|
| `systemctl status qemu-guest-agent` | `sudo launchctl list com.macos.guest-agent` |
| `systemctl start qemu-guest-agent` | `sudo launchctl start com.macos.guest-agent` |
| `systemctl stop qemu-guest-agent` | `sudo launchctl stop com.macos.guest-agent` |
| `systemctl restart qemu-guest-agent` | `sudo launchctl stop com.macos.guest-agent && sudo launchctl start com.macos.guest-agent` |
| `journalctl -u qemu-guest-agent -f` | `tail -f /var/log/mac-guest-agent.log` |

Uninstall:
```bash
sudo mac-guest-agent --uninstall
```

## Configuration

Configuration file format is compatible with the Linux `/etc/qemu/qemu-ga.conf`:

```bash
sudo mkdir -p /etc/qemu
sudo cp configs/qemu-ga.conf /etc/qemu/qemu-ga.conf
```

```ini
[general]
daemonize = 0
method = virtio-serial
path = /dev/cu.org.qemu.guest_agent.0
logfile = /var/log/mac-guest-agent.log
verbose = 0
# block-rpcs = guest-exec,guest-set-user-password
```

## CLI Flags

Flags are compatible with the Linux `qemu-ga` binary:

```
  -d, --daemonize        Run as daemon
  -m, --method METHOD    Transport method [default: virtio-serial]
  -p, --path PATH        Device/socket path [default: auto-detect]
  -l, --logfile PATH     Log file path [default: stderr]
  -f, --pidfile PATH     PID file path
  -v, --verbose          Enable debug logging
  -V, --version          Show version
  -b, --block-rpcs LIST  Comma-separated RPCs to disable
  -a, --allow-rpcs LIST  Comma-separated RPCs to allow
  -c, --config PATH      Config file [default: /etc/qemu/qemu-ga.conf]
  -D, --dump-conf        Print effective configuration
  -t, --test             Test mode (stdin/stdout, no QEMU needed)
  -h, --help             Show help

macOS-specific:
      --install          Install as LaunchDaemon service
      --uninstall        Uninstall LaunchDaemon service
```

## Supported Commands

44 commands matching the official QEMU Guest Agent protocol:

| Category | Commands |
|---|---|
| **Protocol** | `guest-ping`, `guest-sync`, `guest-sync-delimited`, `guest-info` |
| **System** | `guest-get-osinfo`, `guest-get-host-name`, `guest-get-timezone`, `guest-get-time`, `guest-set-time`, `guest-get-users`, `guest-get-load` |
| **Power** | `guest-shutdown`, `guest-suspend-disk`, `guest-suspend-ram`, `guest-suspend-hybrid` |
| **CPU & Memory** | `guest-get-vcpus`, `guest-set-vcpus`, `guest-get-memory-blocks`, `guest-get-memory-block-info`, `guest-set-memory-blocks`, `guest-get-cpustats` |
| **Disk & FS** | `guest-get-disks`, `guest-get-fsinfo`, `guest-get-diskstats`, `guest-fsfreeze-status`, `guest-fsfreeze-freeze`, `guest-fsfreeze-freeze-list`, `guest-fsfreeze-thaw`, `guest-fstrim` |
| **Network** | `guest-network-get-interfaces` |
| **File I/O** | `guest-file-open`, `guest-file-close`, `guest-file-read`, `guest-file-write`, `guest-file-seek`, `guest-file-flush` |
| **Exec** | `guest-exec`, `guest-exec-status` |
| **SSH** | `guest-ssh-get-authorized-keys`, `guest-ssh-add-authorized-keys`, `guest-ssh-remove-authorized-keys` |
| **User** | `guest-set-user-password` |

## Proxmox VE

After installing the agent in a macOS VM, enable the guest agent on the PVE host:

```bash
qm set <vmid> --agent 1
```

Then verify from the PVE host:

```bash
qm agent <vmid> ping
qm agent <vmid> get-osinfo
qm agent <vmid> network-get-interfaces
qm agent <vmid> get-host-name
```

PVE will also use the agent for graceful shutdown and filesystem freeze during backups.

## Compatibility

| Binary | Arch | Min OS | Max OS |
|---|---|---|---|
| `mac-guest-agent-darwin-amd64` | x86_64 | Mac OS X 10.6 Snow Leopard | Current + future |
| `mac-guest-agent-darwin-arm64` | arm64 | macOS 11.0 Big Sur | Current + future |
| `mac-guest-agent-darwin-universal` | x86_64 + arm64 | 10.6 / 11.0 | Current + future |

Tested on: macOS Tahoe 26.3 (arm64), macOS Tahoe 26.3 (x86_64 via Rosetta 2), Mac OS X El Capitan 10.11.6 (x86_64 native).

## Building from Source

Requires Xcode Command Line Tools (`xcode-select --install`).

```bash
git clone https://github.com/mav2287/mac-guest-agent.git
cd mac-guest-agent
make build              # Current architecture
make build-x86_64      # x86_64 targeting 10.6+
make build-arm64       # arm64 targeting 11.0+
make build-universal   # Fat binary
make install           # Build + install service
```

## Testing

```bash
make test                                           # Automated tests
./tests/run_tests.sh ./build/mac-guest-agent        # Full test suite
./tests/safe_test.sh ./build/mac-guest-agent        # Read-only safe test (for production VMs)
mac-guest-agent --test --verbose                    # Interactive test mode
```

## File Locations

| File | Path |
|---|---|
| Binary | `/usr/local/bin/mac-guest-agent` |
| LaunchDaemon | `/Library/LaunchDaemons/com.macos.guest-agent.plist` |
| Config | `/etc/qemu/qemu-ga.conf` |
| Log | `/var/log/mac-guest-agent.log` |
| PID | `/var/run/qemu-ga.pid` |

## License

MIT
