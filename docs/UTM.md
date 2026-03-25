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

### 1. Add a Serial Device in UTM

1. Open the VM settings in UTM
2. Go to **Devices** (or **Serial** section depending on UTM version)
3. Add a **Serial** device with **VirtIO** type
4. The VM will get `/dev/cu.virtio` and `/dev/tty.virtio`

### 2. Install the Agent

```bash
# Inside the macOS VM
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

### 3. Verify

```bash
# Inside the VM
sudo mac-guest-agent --self-test
```

The self-test should show:
```
  PASS  VirtIO serial device: /dev/cu.virtio
```

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
# Check what serial devices exist
ls -la /dev/cu.virtio* /dev/tty.virtio* 2>/dev/null
ls -la /dev/cu.serial* /dev/tty.serial* 2>/dev/null

# Run self-test to see what's detected
sudo mac-guest-agent --self-test
```

If no VirtIO device appears:
1. Verify the Serial device is added in UTM's VM settings
2. Verify it's set to **VirtIO** type (not Legacy)
3. Restart the VM after adding the device

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

UTM on Apple Silicon uses Apple's Virtualization.framework for arm64 macOS guests. The guest agent works with both:

- **Virtualization.framework VMs** (arm64 macOS on arm64 host) — uses `/dev/cu.virtio`
- **QEMU-emulated VMs** (x86_64 macOS via emulation) — uses standard QEMU serial paths

For arm64 VMs, use the `mac-guest-agent-darwin-arm64` binary. For emulated x86_64 VMs, use `mac-guest-agent-darwin-amd64`.
