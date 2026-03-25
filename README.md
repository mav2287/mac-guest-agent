# macOS QEMU Guest Agent

A native QEMU Guest Agent for macOS and OS X virtual machines. The missing guest agent for Mac — enables Proxmox VE, libvirt, plain QEMU, and UTM to manage macOS guests through the standard QGA protocol.

45 commands, real filesystem freeze, zero dependencies. Supports Mac OS X 10.4 Tiger through macOS 26 Tahoe. Works as both a macOS guest agent and an OS X guest agent for legacy VMs.

## Quick Start (Proxmox VE)

**1. On the PVE host:**
```bash
qm set <vmid> --agent enabled=1,type=isa
qm stop <vmid> && sleep 5 && qm start <vmid>
```

**2. In the macOS VM:**
```bash
# Download binary (from a modern machine if VM can't reach GitHub)
curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-amd64

# Install
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

**3. Verify:**
```bash
# From PVE host
qm agent <vmid> ping
qm agent <vmid> get-osinfo

# From inside VM
sudo mac-guest-agent --self-test
```

## How It Works

The agent communicates via an **ISA serial port** (16550 UART) using Apple's built-in `Apple16X50Serial.kext` driver, present on every macOS since 10.4. No custom kexts, no SIP changes, no code signing.

On Big Sur 11.0+, VirtIO serial also works via Apple's native `AppleVirtIO.kext`. The agent auto-detects the available transport and prefers VirtIO when present.

## Compatibility

| Binary | Arch | Min macOS |
|---|---|---|
| `mac-guest-agent-i386` | i386 | 10.4 Tiger |
| `mac-guest-agent-darwin-amd64` | x86_64 | 10.6 Snow Leopard |
| `mac-guest-agent-darwin-arm64` | arm64 | 11.0 Big Sur |
| `mac-guest-agent-darwin-universal` | x86_64 + arm64 | 10.6 / 11.0 |

ISA serial driver (`Apple16X50Serial.kext`) verified present with identical PCI class match on every macOS from 10.4 Tiger (2005) through 26.3 Tahoe (2026). See the [compatibility matrix](docs/COMPATIBILITY.md) for per-version evidence.

## Service Management

```bash
sudo launchctl list com.macos.guest-agent     # Status
sudo launchctl start com.macos.guest-agent    # Start
sudo launchctl stop com.macos.guest-agent     # Stop
tail -f /var/log/mac-guest-agent.log          # Logs
sudo mac-guest-agent --uninstall              # Remove
```

## Building from Source

```bash
make build              # Current architecture
make build-x86_64      # Intel (10.6+)
make build-arm64       # Apple Silicon (11.0+)
make build-universal   # Fat binary
make test              # Run all tests
```

## Documentation

| Guide | Description |
|---|---|
| **[Proxmox VE](docs/PVE.md)** | VM settings, backup config, TRIM, security profiles, troubleshooting |
| **[libvirt / virt-manager](docs/LIBVIRT.md)** | Domain XML, virsh commands, quiesced snapshots, troubleshooting |
| **[UTM](docs/UTM.md)** | Local macOS VM automation, utmctl integration, CI/CD workflows |
| [All Platforms](docs/PLATFORMS.md) | Plain QEMU setup, transport priority |
| [Commands](docs/COMMAND_STATUS.md) | All 45 commands with status, Linux parity, privilege requirements |
| [Compatibility](docs/COMPATIBILITY.md) | Support tiers, kext timeline, verification evidence per version |
| [Backup & Freeze](docs/BACKUP.md) | Filesystem freeze, APFS snapshots, hook scripts, TRIM |
| [Security](SECURITY.md) | Trust model, recommended profiles, freeze-state restrictions |
| [Architecture](docs/ARCHITECTURE.md) | Data flow, protocol spec, macOS API usage |
| [CLI Reference](docs/CLI.md) | All flags, config file format, examples |
| [Changelog](CHANGELOG.md) | Release history |
| [Contributing](CONTRIBUTING.md) | Build, style, testing guidelines |

## License

MIT
