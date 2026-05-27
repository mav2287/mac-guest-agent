# UTM Guide

Complete guide for using the macOS guest agent with [UTM](https://mac.getutm.app/) — the popular QEMU wrapper for macOS.

## When to Use This

UTM is the right choice when you need:
- **Local macOS VM automation** on a Mac host (CI/CD, testing, development)
- **Headless or semi-headless guest management** without VNC/screen sharing
- **Programmatic control** of macOS VMs via AppleScript, Shortcuts, or CLI
- **Legacy macOS environments** for testing older software

The guest agent turns a UTM VM from a "window with a desktop" into a **managed virtual machine** you can query, control, and back up programmatically.

## Setup

> **Transport choice.** As of v2.5.0 the agent supports **ISA serial only** across every host class (Proxmox VE, libvirt, raw QEMU, UTM Emulate backend). VirtIO transport was removed — see the [v2.5.0 BREAKING entry in CHANGELOG.md](../CHANGELOG.md) for the migration path from v2.4.x. The ISA-only contract avoids the VZ-backed conflict described in [Apple Silicon Notes](#apple-silicon-notes) below, gives one install/launchd config everywhere, and survives moving a disk image between host classes without reinstall.

### 1. Add a Serial Device in UTM (QEMU backend)

1. Open the VM settings in UTM
2. Go to **Devices** (or **Serial** section depending on UTM version)
3. Add a **Serial** device:
   - **Interface:** *QemuGuestAgent* (UTM provisions a host-side Unix socket the agent can be reached on — same mechanism `utmctl exec` uses internally and what `scripts/verify.sh --transport utm` talks to)
   - For a manually managed socket, leave the Interface as a generic serial and set **Mode: Unix socket** with a known path
4. The VM will get `/dev/cu.serial1` (or similar `/dev/cu.serial*`)

> Older UTM guidance (and v2.4.x of this agent) suggested adding a VirtIO interface and using `/dev/cu.virtio`. **That path was removed in v2.5.0.** If you upgraded from v2.4.x and previously had VirtIO Serial configured, the agent will refuse to start with a "No ISA serial device found" message pointing here; reconfigure the VM with a Serial device whose Interface is **QemuGuestAgent** (ISA-backed) and restart. On Virtualization.framework-backed macOS guests the VirtIO console is claimed by Apple's own 18-command agent regardless ([Apple Silicon Notes](#apple-silicon-notes)), so the v2.5.0 ISA-only contract also closes that quiet-failure mode.

### 2. Install the Agent

```bash
# Inside the macOS VM
sudo cp mac-guest-agent-darwin-universal /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

### 3. Verify (in-VM quick check)

```bash
# Inside the VM
sudo mac-guest-agent --self-test
```

The self-test should show:
```
  PASS  ISA serial device: /dev/cu.serial1 (r=yes w=yes)
```

If your VM was set up with a VirtIO Serial Interface (older UTM guidance, or any v2.4.x install that worked via the VirtIO fallback), the agent will refuse to start in v2.5.0 with a "Found VirtIO serial device but VirtIO transport was removed" message — reconfigure the Serial device's Interface to **QemuGuestAgent** (ISA-backed) and fully stop/start the VM. See [CHANGELOG v2.5.0 BREAKING](../CHANGELOG.md) for the migration steps.

### 4. Verify end-to-end from the host (`scripts/verify.sh`)

For a single end-to-end pass — environment capture, agent communication, freeze/thaw cycles, and the in-VM `--self-test-json` / `--safe-test-json` diagnostics, all driven from your macOS host — use `scripts/verify.sh`:

```bash
# Pick a name shown in `utmctl list`
curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh | bash -s -- --transport utm "macOS Dev" | tee verify.txt
```

How the UTM transport works:

- **Socket discovery.** `verify.sh` reads `~/Library/Containers/com.utmapp.UTM/Data/Documents/<VM Name>.utm/config.plist`, finds the Serial entry where `Interface == "QemuGuestAgent"`, and uses its `Path` (the Unix socket UTM provisions for the QGA serial). `utmctl exec` uses the same socket under the hood, but `utmctl` itself has no arbitrary-QGA subcommand — talking to the socket directly is what gives us full PVE-parity coverage (host-driven ping / get-osinfo / fsfreeze round-trip / get-memory).
- **Prerequisite.** Your UTM VM needs a Serial device with **Interface: QemuGuestAgent** (Edit → Devices → Serial → Add → Interface: QemuGuestAgent). This is the same setup that makes `utmctl exec` work; if you've used `utmctl exec` against this VM successfully, the socket is already there.
- **Run as the desktop user.** UTM's socket is owned by the user that owns the .utm bundle. **Don't sudo** — `verify.sh` will refuse to run as root in UTM mode.
- **`--qga-socket PATH` override.** If your UTM install diverges from the default bundle layout (custom paths, older UTM versions with a different plist schema, etc.), pass `--qga-socket /path/to/socket.sock` and `verify.sh` will skip discovery and talk to that socket directly.

If discovery fails — bundle not found, or the plist exists but has no QemuGuestAgent serial — the script errors with the exact UTM GUI steps to fix it. It never writes to the `.utm` bundle.

`verify.sh --help` lists the rest of the flags (`--no-freeze`, `--no-in-vm`, `--no-env-capture`, `--no-appendix`, `--no-redact`, `--freeze-cycles N`, `--agent-path`, `--log-path`, `--exec-timeout`). PII (IPv4, MAC, supplied identifier) is redacted by default.

## Guest Agent vs utmctl

UTM provides its own CLI tool (`utmctl`) and AppleScript interface. Here's when to use each:

| Task | Guest Agent | utmctl |
|---|---|---|
| Get VM IP address | `guest-network-get-interfaces` | `utmctl ip-address <vm>` |
| Run a command | `guest-exec` | `utmctl exec <vm> -- cmd` |
| Transfer files | `guest-file-open/read/write` | `utmctl file pull/push` |
| Get OS info | `guest-get-osinfo` | Not available |
| Filesystem freeze | `guest-fsfreeze-freeze` | Not available |
| Shutdown/reboot | `guest-shutdown` | `utmctl stop <vm>` (force) |
| CPU/memory stats | `guest-get-cpustats`, `guest-get-memory-blocks` | Not available |
| Disk info | `guest-get-disks`, `guest-get-fsinfo` | Not available |
| Set user password | `guest-set-user-password` | Not available |
| SSH key management | `guest-ssh-add/remove-authorized-keys` | Not available |

**Use the guest agent when you need:**
- System introspection (OS info, hardware stats, disk info, network routes)
- Filesystem freeze for consistent backups
- Detailed network information beyond just IP addresses
- User management (passwords, SSH keys)
- Fine-grained command execution with output capture

**Use utmctl when you need:**
- Quick file transfers (`utmctl file push/pull`)
- Simple command execution
- VM lifecycle management (start, stop, suspend)
- AppleScript/Shortcuts integration

**Use both together for:**
- Automated testing: utmctl for VM lifecycle, agent for in-guest verification
- Backup workflows: agent freeze + UTM snapshot
- CI/CD pipelines: utmctl for orchestration, agent for environment setup

## Headless VM Automation

### Start a VM headlessly and query it

```bash
# Start the VM in background
utmctl start "macOS Dev"

# Wait for agent to come up
for i in $(seq 1 30); do
    IP=$(utmctl ip-address "macOS Dev" 2>/dev/null)
    [ -n "$IP" ] && break
    sleep 2
done

echo "VM IP: $IP"

# Use SSH + agent for deeper queries
ssh user@$IP 'sudo mac-guest-agent --self-test-json' | python3 -m json.tool
```

### Automated backup with freeze

```bash
VM="macOS Dev"

# Freeze the filesystem (via SSH to the agent, or via the serial channel)
ssh user@$(utmctl ip-address "$VM") 'echo "{\"execute\":\"guest-fsfreeze-freeze\"}" | sudo mac-guest-agent --test 2>/dev/null'

# Take UTM snapshot
utmctl snapshot "$VM" --name "backup-$(date +%Y%m%d)"

# Thaw
ssh user@$(utmctl ip-address "$VM") 'echo "{\"execute\":\"guest-fsfreeze-thaw\"}" | sudo mac-guest-agent --test 2>/dev/null'
```

### CI/CD Pipeline Example

```bash
#!/bin/bash
# Build and test on a macOS VM via UTM

VM="CI Runner"
utmctl start "$VM"

# Wait for boot
sleep 30
IP=$(utmctl ip-address "$VM")

# Copy build artifacts
utmctl file push "$VM" ./build/myapp /tmp/myapp

# Run tests via agent
ssh user@$IP 'cd /tmp && ./myapp --run-tests'

# Collect results
utmctl file pull "$VM" /tmp/test-results.xml ./results/

# Get system info for the test report
ssh user@$IP 'sudo mac-guest-agent --self-test-json' > ./results/system-info.json

utmctl stop "$VM"
```

## Using the Serial Console Directly

UTM exposes the serial device as a pseudo-terminal on the host. You can connect to the agent directly without SSH:

```bash
# Find the serial device on the host
# UTM typically creates a pty — check UTM's serial console window or logs

# Send a command directly through the serial port
echo '{"execute":"guest-ping"}' > /dev/cu.usbmodem*  # Path varies
```

For most use cases, SSH is simpler than raw serial access. The serial channel is primarily for the agent's internal communication with UTM/QEMU, not for direct user interaction.

## Troubleshooting

### Agent not finding the serial device

```bash
# Check what serial devices exist. v2.5.0+ only uses ISA paths;
# if a VirtIO device shows here, the agent will refuse to start
# and tell you to reconfigure (see CHANGELOG v2.5.0 BREAKING).
ls -la /dev/cu.serial* /dev/tty.serial* 2>/dev/null
ls -la /dev/cu.virtio* /dev/tty.virtio* 2>/dev/null

# Run self-test to see what's detected
sudo mac-guest-agent --self-test
```

If no serial device appears:
1. Verify a Serial device is added in UTM's VM settings (Devices → Serial)
2. For the supported path, set **Interface: QemuGuestAgent** (gives an ISA-backed `/dev/cu.serial*` plus a host-side socket UTM manages)
3. Fully stop and start the VM after adding the device — a reboot inside the guest is not enough; QEMU needs to recreate the device on launch
4. If you're on UTM's Virtualization.framework backend (arm64 macOS guest on an Apple Silicon host), see [Apple Silicon Notes](#apple-silicon-notes) — VirtIO console is claimed by Apple's own agent on VZ-backed VMs, and the recommended fix is switching the VM to UTM's QEMU backend

### utmctl not finding the VM

```bash
# List all VMs
utmctl list

# UTM must be running for utmctl to work
open -a UTM
```

### Agent works but utmctl exec doesn't

`utmctl exec` uses its own mechanism (SPICE agent or serial), not the QEMU guest agent. They are independent. The guest agent provides a superset of capabilities via the QGA protocol.

## Apple Silicon Notes

UTM on Apple Silicon supports two backends for macOS guests, and the transport story is different for each:

### QEMU backend (recommended)

Works the same way as on Intel hosts: ISA serial via the **QemuGuestAgent** interface (or any other Mode that surfaces an ISA UART to the guest). The agent loads its arm64 slice on arm64 guests and its x86_64 slice on x86_64-emulated guests — same `mac-guest-agent-darwin-universal` binary, dyld picks the slice at load time.

This is the supported path.

### Apple Virtualization.framework backend (VZ)

UTM's "Virtualize" mode for arm64 macOS guests uses Apple's `Virtualization.framework` directly rather than QEMU. The VZ runtime provisions a virtio-console channel automatically and **Apple's own `AppleQEMUGuestAgent` claims that channel** as part of the guest OS bootstrap (matched via `AppleVirtIOAgentDevice` / `applevirtio.console`). That agent ships ~18 basic commands and does not implement freeze. Adding `mac-guest-agent` to the same VirtIO channel either fails to open it or conflicts with Apple's daemon, depending on launch order.

Workarounds, in order of preference:

1. **Switch the VM to UTM's QEMU backend.** Slower (no Virtualization.framework acceleration on arm64 macOS guests, so guest performance drops noticeably) but gets you ISA serial, the supported transport, and the full 45-command surface including freeze. This is the recommended fix if you need this agent's full feature set.
2. **Live with Apple's 18-command agent.** If you only need ping / get-osinfo / network / shutdown and don't need freeze or guest-exec, Apple's built-in agent is fine — just don't install this one.
3. **Custom advanced configuration.** Disabling Apple's daemon manually or convincing VZ to expose an additional serial channel is possible but undocumented by Apple and likely to break across macOS updates. Not supported.

This conflict only affects arm64 macOS guests on Apple Silicon hosts using UTM's Virtualize backend. Intel macOS guests on any host, and arm64 macOS guests under UTM's QEMU backend, are unaffected.
