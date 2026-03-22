# macOS QEMU Guest Agent

A native QEMU Guest Agent for macOS, written in C. Enables hypervisors (Proxmox VE, libvirt, plain QEMU) to manage macOS virtual machines through the standard QGA protocol.

**Supports Mac OS X 10.4 Tiger through macOS Tahoe and beyond.** Works on any macOS VM — OpenCore, Hackintosh, real Apple hardware in Proxmox, doesn't matter.

## How It Works

The agent communicates with the hypervisor through an **ISA serial port** — a standard 16550 UART that macOS has built-in drivers for (`Apple16X50Serial.kext`) since day one. No custom kernel extensions, no SIP issues, no code signing required.

## Setup

### 1. Configure PVE Host (one-time)

Set the guest agent to use ISA serial instead of the default virtio-serial:

```bash
qm set <vmid> --agent enabled=1,type=isa
```

Then restart the VM (QEMU args change requires a full stop/start):

```bash
qm stop <vmid> && sleep 5 && qm start <vmid>
```

> **Why `type=isa`?** The default `agent: 1` uses virtio-serial, which only works on macOS Big Sur 11.0+ (where Apple ships a built-in VirtIO driver). The `type=isa` option uses a standard serial port that macOS supports natively on **every version** from 10.4 to current. We recommend `type=isa` for universal compatibility.

### Already running Big Sur or newer?

If your VM runs **macOS 11.0 (Big Sur) or later**, the default `agent: 1` (virtio-serial) also works — Apple's built-in `AppleVirtIO.kext` handles it. The agent auto-detects both device types:

| PVE Setting | macOS Driver | Works On |
|---|---|---|
| `type=isa` (recommended) | Apple16X50Serial (built-in) | All macOS 10.4+ |
| default (type=virtio) | AppleVirtIO (built-in) | Big Sur 11.0+ only |

Either way, just install the agent binary. No double-dipping — PVE creates one device type or the other, never both.

### 2. Install the Agent in the macOS VM

**From another machine on the network** (old macOS can't reach GitHub directly due to TLS):

```bash
# Download the binary on a modern machine
curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-amd64

# Copy to the VM
scp mac-guest-agent user@<vm-ip>:/tmp/
```

**On the macOS VM**:

```bash
sudo cp /tmp/mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

### 3. Verify

From the **PVE host**:

```bash
qm agent <vmid> ping
qm agent <vmid> get-osinfo
qm agent <vmid> network-get-interfaces
```

## Service Management

| Linux (systemd) | macOS (launchctl) |
|---|---|
| `systemctl status qemu-guest-agent` | `sudo launchctl list com.macos.guest-agent` |
| `systemctl start qemu-guest-agent` | `sudo launchctl start com.macos.guest-agent` |
| `systemctl stop qemu-guest-agent` | `sudo launchctl stop com.macos.guest-agent` |
| `journalctl -u qemu-guest-agent -f` | `tail -f /var/log/mac-guest-agent.log` |

Uninstall:

```bash
sudo mac-guest-agent --uninstall
```

## CLI Flags

Compatible with the Linux `qemu-ga`:

```
  -d, --daemonize        Run as daemon
  -m, --method METHOD    Transport method [default: virtio-serial]
  -p, --path PATH        Device/socket path [default: auto-detect]
  -l, --logfile PATH     Log file path
  -f, --pidfile PATH     PID file path
  -v, --verbose          Debug logging
  -V, --version          Show version
  -b, --block-rpcs LIST  Comma-separated RPCs to disable
  -a, --allow-rpcs LIST  Comma-separated RPCs to allow
  -c, --config PATH      Config file [default: /etc/qemu/qemu-ga.conf]
  -D, --dump-conf        Print configuration
  -t, --test             Test mode (stdin/stdout, no QEMU needed)
  -h, --help             Show help
      --install          Install as LaunchDaemon
      --uninstall        Uninstall LaunchDaemon
```

## Configuration File

Optional. Compatible with Linux `/etc/qemu/qemu-ga.conf`:

```ini
[general]
daemonize = 0
method = virtio-serial
path = /dev/cu.serial1
logfile = /var/log/mac-guest-agent.log
verbose = 0
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

## Compatibility

| Binary | Arch | Min OS | Max OS |
|---|---|---|---|
| `mac-guest-agent-darwin-amd64` | x86_64 | Mac OS X 10.6 Snow Leopard | Current + future |
| `mac-guest-agent-darwin-arm64` | arm64 | macOS 11.0 Big Sur | Current + future |
| `mac-guest-agent-darwin-universal` | x86_64 + arm64 | 10.6 / 11.0 | Current + future |

Tested on:
- macOS Tahoe 26.3 (arm64 native + x86_64 Rosetta 2)
- Mac OS X El Capitan 10.11.6 (x86_64 native, Proxmox VE)

## Building from Source

```bash
git clone https://github.com/mav2287/mac-guest-agent.git
cd mac-guest-agent
make build              # Current architecture
make build-x86_64      # Intel, targeting 10.6+
make build-arm64       # Apple Silicon, targeting 11.0+
make build-universal   # Fat binary
make install           # Build + install service
```

## Testing

```bash
make test                                        # Automated quick tests
./tests/run_tests.sh ./build/mac-guest-agent     # Full test suite
./tests/safe_test.sh ./build/mac-guest-agent     # Read-only safe test (for production VMs)
mac-guest-agent -t -v                            # Interactive test mode
```

## How It Works (Technical)

1. PVE sets `agent: enabled=1,type=isa` which tells QEMU to create an ISA 16550 serial port connected to the guest agent socket
2. macOS sees the serial port via its built-in `Apple16X50Serial.kext` driver and creates `/dev/cu.serial1`
3. Our agent opens `/dev/cu.serial1`, sets raw mode, and speaks the QGA JSON protocol
4. PVE communicates through the QEMU socket, QEMU bridges to the serial port, the agent responds

No custom kernel extensions. No VirtIO drivers. No SIP modifications. Works on every macOS version from 10.4 to current.

## File Locations

| File | Path |
|---|---|
| Binary | `/usr/local/bin/mac-guest-agent` |
| LaunchDaemon | `/Library/LaunchDaemons/com.macos.guest-agent.plist` |
| Config (optional) | `/etc/qemu/qemu-ga.conf` |
| Log | `/var/log/mac-guest-agent.log` |

## License

MIT
