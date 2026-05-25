# macOS QEMU Guest Agent

A native QEMU Guest Agent for macOS and OS X virtual machines. The missing guest agent for Mac — enables Proxmox VE, libvirt, plain QEMU, and UTM to manage macOS guests through the standard QGA protocol.

45 commands, real filesystem freeze, zero dependencies. Supports Mac OS X 10.4 Tiger through macOS 26 Tahoe. Works as both a macOS guest agent and an OS X guest agent for legacy VMs.

## Quick Start (Proxmox VE)

**1. On the PVE host — enable the guest agent:**
```bash
qm set <vmid> --agent enabled=1,type=isa
qm stop <vmid> && sleep 5 && qm start <vmid>
```

> **Why `type=isa`?** macOS guests run in two distinct environments: under **Apple's Virtualization.framework** (UTM, `vz_run`, anything backed by `VZVirtualMachine`), and under **plain QEMU/KVM** (Proxmox, libvirt, raw QEMU — typically with OpenCore as the bootloader). On Virtualization.framework hosts, Apple's own `AppleQEMUGuestAgent` is IOKit-launched on the VirtIO console channel and would conflict with ours. On Proxmox/QEMU/OpenCore hosts, Apple's agent never launches (its `AppleVirtIOAgentDevice` IOKit match is only set by `applevirtio.console`, which only loads on VZ). We use ISA serial universally to keep one transport across both host types — no host-detection logic, no per-environment conditional registration — at the cost of using a slightly older transport, which every macOS version from 10.4 onwards supports natively via `Apple16X50Serial.kext`. See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md#isa-serial-transport--why) for the full evidence.

**2. In the macOS VM:**
```bash
# Download the universal binary — one slice (i386 / x86_64 / arm64) loads at runtime.
# Covers macOS 10.4 Tiger through 26 Tahoe.
# Note: on Tiger / Leopard / older Snow Leopard guests, the VM's TLS stack
# usually cannot reach GitHub directly. Download on a modern machine and
# transfer the file (scp / shared folder / USB).
curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-universal

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

> **Note:** macOS Big Sur and newer ship Apple's own `AppleQEMUGuestAgent` (18 commands, no filesystem freeze). It only launches on Apple Virtualization.framework hosts — its IOKit match (`AppleVirtIOAgentDevice`) is set by the `applevirtio.console` driver, which doesn't load under QEMU/OpenCore — so on Proxmox the VirtIO channel is actually free. We still default to ISA for symmetry across both host classes (see the [Quick Start callout](#quick-start-proxmox-ve) for the trade-off). Our agent provides 45 commands plus a real per-filesystem freeze dispatch — see [docs/design/FREEZE_SEMANTICS.md](docs/design/FREEZE_SEMANTICS.md) for what "freeze" actually does per `f_fstypename`.

## Compatibility

**Single download:** `mac-guest-agent-darwin-universal` (i386 + x86_64 + arm64, covers macOS 10.4 Tiger through 26 Tahoe). dyld picks the right slice at load time.

ISA serial driver (`Apple16X50Serial.kext`) verified present with identical PCI class match on every macOS from 10.4 Tiger (2005) through 26.3 Tahoe (2026). See the [compatibility matrix](docs/COMPATIBILITY.md) for per-version evidence.

> **Tiger / Leopard (10.4 / 10.5):** `/usr/local/bin` is not in the default shell PATH. Invoke the binary by its absolute path (`sudo /usr/local/bin/mac-guest-agent ...`) or add it to PATH for the session: `export PATH=/usr/local/bin:$PATH`. The LaunchDaemon itself uses the absolute path and is unaffected.

## If the agent doesn't start

Open an issue at https://github.com/mav2287/mac-guest-agent/issues/new with as much of the following as you can collect. Items 1-3 work even when the binary won't launch (dyld rejection); items 4-6 require the binary to start successfully.

**Loader-safe (no execution needed):**

1. `sw_vers` — macOS version, build
2. `file /usr/local/bin/mac-guest-agent` — confirms it's a Mach-O fat binary
3. `lipo -info /usr/local/bin/mac-guest-agent` — shows the slice list

**If the binary starts:**

4. `mac-guest-agent --version`
5. `mac-guest-agent --self-test-json` — pipe to a file or `grep selected_arch` directly (do NOT pipe through `python -m json.tool` on Tiger/Leopard — those versions lack a reliable stdlib `json` module)
6. `tail -50 /var/log/mac-guest-agent.log`

The universal binary is the supported install path; if it doesn't load on your specific host configuration we want to fix it, not work around it.

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
| [Freeze Semantics](docs/design/FREEZE_SEMANTICS.md) | What "freeze" actually does per `f_fstypename`; divergences from upstream QGA |
| [Security](SECURITY.md) | Trust model, recommended profiles, freeze-state restrictions |
| [Architecture](docs/ARCHITECTURE.md) | Data flow, protocol spec, macOS API usage |
| [CLI Reference](docs/CLI.md) | All flags, config file format, examples |
| [Changelog](CHANGELOG.md) | Release history |
| [Contributing](CONTRIBUTING.md) | Build, style, testing guidelines |

## License

MIT
