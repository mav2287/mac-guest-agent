# libvirt / virt-manager Guide

Complete guide for running macOS VMs with guest agent support on libvirt and virt-manager.

## Domain XML Configuration

### VirtIO Channel (Big Sur 11.0+)

The preferred transport on Big Sur and newer. Uses Apple's native `AppleVirtIO.kext`.

```xml
<devices>
  <!-- Guest agent channel -->
  <channel type='unix'>
    <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
    <target type='virtio' name='org.qemu.guest_agent.0'/>
  </channel>

  <!-- Recommended device settings for Big Sur+ -->
  <disk type='file' device='disk'>
    <driver name='qemu' type='qcow2' cache='writeback' discard='unmap'/>
    <source file='/var/lib/libvirt/images/macos.qcow2'/>
    <target dev='vda' bus='virtio'/>
  </disk>

  <interface type='network'>
    <source network='default'/>
    <model type='virtio'/>
  </interface>
</devices>
```

Inside the VM, the agent finds `/dev/cu.org.qemu.guest_agent.0`.

### ISA Serial (All macOS 10.4+)

Required for pre-Big Sur. Uses Apple's built-in `Apple16X50Serial.kext`.

```xml
<devices>
  <!-- Guest agent via ISA serial -->
  <serial type='unix'>
    <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
    <target type='isa-serial' port='0'/>
  </serial>

  <!-- SATA disk for pre-Big Sur (no VirtIO block support) -->
  <disk type='file' device='disk'>
    <driver name='qemu' type='qcow2' cache='writeback' discard='unmap'/>
    <source file='/var/lib/libvirt/images/macos.qcow2'/>
    <target dev='sda' bus='sata'/>
  </disk>

  <interface type='network'>
    <source network='default'/>
    <model type='e1000'/>
  </interface>
</devices>
```

Inside the VM, the agent finds `/dev/cu.serial1`.

### Choosing a Transport

| macOS Version | Transport | XML Element | Device in VM |
|---|---|---|---|
| 10.4–10.15 | ISA serial | `<serial type='unix'>` | `/dev/cu.serial1` |
| 11.0+ | VirtIO channel | `<channel type='virtio'>` | `/dev/cu.org.qemu.guest_agent.0` |

The agent auto-detects the available transport. VirtIO is preferred when both are present.

## Guest Agent Commands via virsh

### Basic Commands

```bash
# Ping the agent
virsh qemu-agent-command macos-vm '{"execute":"guest-ping"}'

# Get OS info
virsh qemu-agent-command macos-vm '{"execute":"guest-get-osinfo"}'

# Get hostname
virsh qemu-agent-command macos-vm '{"execute":"guest-get-host-name"}'

# Get network interfaces (IP addresses)
virsh qemu-agent-command macos-vm '{"execute":"guest-network-get-interfaces"}'

# Get routing table
virsh qemu-agent-command macos-vm '{"execute":"guest-network-get-route"}'

# Get system load
virsh qemu-agent-command macos-vm '{"execute":"guest-get-load"}'
```

### Shutdown and Reboot

```bash
# Graceful shutdown
virsh qemu-agent-command macos-vm '{"execute":"guest-shutdown","arguments":{"mode":"powerdown"}}'

# Reboot
virsh qemu-agent-command macos-vm '{"execute":"guest-shutdown","arguments":{"mode":"reboot"}}'

# Halt
virsh qemu-agent-command macos-vm '{"execute":"guest-shutdown","arguments":{"mode":"halt"}}'
```

### File Operations

```bash
# Read a file from the guest
HANDLE=$(virsh qemu-agent-command macos-vm '{"execute":"guest-file-open","arguments":{"path":"/etc/hosts","mode":"r"}}' | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])")
virsh qemu-agent-command macos-vm "{\"execute\":\"guest-file-read\",\"arguments\":{\"handle\":$HANDLE,\"count\":4096}}"
virsh qemu-agent-command macos-vm "{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":$HANDLE}}"

# Execute a command in the guest
PID=$(virsh qemu-agent-command macos-vm '{"execute":"guest-exec","arguments":{"path":"/bin/hostname"}}' | python3 -c "import json,sys; print(json.load(sys.stdin)['return']['pid'])")
sleep 1
virsh qemu-agent-command macos-vm "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":$PID}}"
```

## Snapshots with Quiesced Freeze

libvirt supports `--quiesce` flag on snapshots, which automatically calls `guest-fsfreeze-freeze` before the snapshot and `guest-fsfreeze-thaw` after.

### Create a Quiesced Snapshot

```bash
# Disk-only snapshot with filesystem quiesce
virsh snapshot-create-as macos-vm snap1 --disk-only --quiesce

# Full snapshot with quiesce
virsh snapshot-create-as macos-vm snap1 --quiesce
```

If the agent is running and responds to ping, `--quiesce` will:
1. Call `guest-fsfreeze-freeze` (runs hooks, creates APFS snapshot, syncs)
2. Take the VM snapshot
3. Call `guest-fsfreeze-thaw` (cleans up, runs thaw hooks)

### Manual Freeze/Thaw

```bash
# Freeze
virsh qemu-agent-command macos-vm '{"execute":"guest-fsfreeze-freeze"}'

# Check status
virsh qemu-agent-command macos-vm '{"execute":"guest-fsfreeze-status"}'

# Take snapshot while frozen
virsh snapshot-create-as macos-vm backup-snap --disk-only

# Thaw
virsh qemu-agent-command macos-vm '{"execute":"guest-fsfreeze-thaw"}'
```

### Verify Freeze Support

```bash
# Check if agent supports freeze
virsh qemu-agent-command macos-vm '{"execute":"guest-info"}' | python3 -c "
import json, sys
info = json.load(sys.stdin)['return']
cmds = {c['name']: c['enabled'] for c in info['supported_commands']}
freeze = cmds.get('guest-fsfreeze-freeze', False)
print(f'Freeze supported: {freeze}')
print(f'Agent version: {info[\"version\"]}')
"
```

## Complete Domain XML Example

A full working domain XML for a macOS Sonoma VM with guest agent:

```xml
<domain type='kvm'>
  <name>macos-sonoma</name>
  <memory unit='GiB'>8</memory>
  <vcpu>4</vcpu>

  <os>
    <type arch='x86_64' machine='q35'>hvm</type>
    <loader readonly='yes' type='pflash'>/usr/share/OVMF/OVMF_CODE.fd</loader>
    <nvram>/var/lib/libvirt/qemu/nvram/macos-sonoma_VARS.fd</nvram>
  </os>

  <features>
    <acpi/>
    <apic/>
  </features>

  <cpu mode='host-passthrough'/>

  <clock offset='utc'>
    <timer name='rtc' tickpolicy='catchup'/>
    <timer name='pit' tickpolicy='delay'/>
    <timer name='hpet' present='no'/>
  </clock>

  <devices>
    <!-- OpenCore ISO -->
    <disk type='file' device='cdrom'>
      <source file='/var/lib/libvirt/images/OpenCore.iso'/>
      <target dev='hdc' bus='ide'/>
      <readonly/>
    </disk>

    <!-- Main disk -->
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2' cache='writeback' discard='unmap'/>
      <source file='/var/lib/libvirt/images/macos-sonoma.qcow2'/>
      <target dev='vda' bus='virtio'/>
    </disk>

    <!-- Network -->
    <interface type='network'>
      <source network='default'/>
      <model type='virtio'/>
    </interface>

    <!-- Guest agent channel -->
    <channel type='unix'>
      <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
      <target type='virtio' name='org.qemu.guest_agent.0'/>
    </channel>

    <!-- Display -->
    <video>
      <model type='vmvga'/>
    </video>
    <graphics type='vnc' port='-1'/>

    <!-- USB for keyboard/mouse -->
    <input type='keyboard' bus='usb'/>
    <input type='mouse' bus='usb'/>
  </devices>
</domain>
```

## Troubleshooting

### "error: Guest agent is not responding"

```bash
# 1. Verify the channel/serial is configured
virsh dumpxml macos-vm | grep -A3 "channel\|serial"

# 2. Check the socket exists
ls -la /var/lib/libvirt/qemu/macos-agent.sock

# 3. Try a direct ping with timeout
virsh qemu-agent-command macos-vm '{"execute":"guest-ping"}' --timeout 10

# 4. Inside the VM, check the agent
sudo launchctl list com.macos.guest-agent
tail -20 /var/log/mac-guest-agent.log
sudo mac-guest-agent --self-test
```

### Serial device not found in VM

If using ISA serial and `/dev/cu.serial1` doesn't appear:

1. Verify the `<serial>` element is in the domain XML (not `<channel>`)
2. Check `system_profiler SPSerialATADataType` in the VM for serial ports
3. Look for `Apple16X50Serial` in `kextstat` output
4. Restart the VM (not just reboot) after XML changes

### VirtIO channel not found in VM

If using VirtIO and `/dev/cu.org.qemu.guest_agent.0` doesn't appear:

1. Verify macOS is 11.0+ (VirtIO requires Big Sur or newer)
2. Check that `AppleVirtIO.kext` is loaded: `kextstat | grep VirtIO`
3. Verify the channel name is exactly `org.qemu.guest_agent.0`
4. Restart the VM after XML changes

### Quiesced snapshot fails

```bash
# Check if freeze works manually
virsh qemu-agent-command macos-vm '{"execute":"guest-fsfreeze-freeze"}'
# Should return: {"return":N} where N is frozen filesystem count
virsh qemu-agent-command macos-vm '{"execute":"guest-fsfreeze-thaw"}'

# If freeze times out, check hook scripts inside the VM
sudo mac-guest-agent --self-test
ls -la /etc/qemu/fsfreeze-hook.d/
```

## Security Profiles

### Recommended: Standard Management

Allows shutdown, freeze, and system queries. Blocks exec, file I/O, SSH, and passwords.

In `/etc/qemu/qemu-ga.conf` inside the VM:
```ini
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-set-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-fsfreeze-freeze,guest-fsfreeze-thaw,guest-network-get-interfaces,guest-network-get-route,guest-shutdown
```

### Minimal: Read-Only

No modifications of any kind:
```ini
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-network-get-interfaces,guest-network-get-route
```
