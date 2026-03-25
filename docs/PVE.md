# Proxmox VE Guide

Complete guide for running macOS VMs with guest agent support on Proxmox VE.

## VM Configuration

### Recommended Settings

| Setting | Value | Why |
|---|---|---|
| Machine | q35 | Required for OpenCore |
| BIOS | OVMF (UEFI) | Required for macOS |
| CPU | host or Nehalem | Nehalem for broadest compatibility |
| Agent | `enabled=1,type=isa` | ISA serial — works on all macOS versions |
| OS Type | other | Prevents PVE from making Linux assumptions |
| Balloon | 0 (disabled) | macOS doesn't support memory ballooning |

### Disk Settings

| macOS Version | Disk Type | Recommended Flags |
|---|---|---|
| 10.4–10.14 | **SATA** | `discard=on,ssd=1` |
| 10.15+ | **VirtIO Block** | `cache=writeback,discard=on,ssd=1` |

- `discard=on` enables TRIM passthrough for thin provisioning
- `ssd=1` tells macOS the disk supports TRIM
- `cache=writeback` improves write performance (safe with battery-backed storage or when backups protect against data loss)

### Network Settings

| macOS Version | Network Adapter |
|---|---|
| 10.4–10.14 | **e1000** |
| 10.15+ | **VirtIO** (or e1000 as fallback) |

### Example PVE Command-Line Setup

```bash
# Create VM
qm create 200 --name macos-vm --memory 8192 --cores 4 \
  --cpu Nehalem --machine q35 --bios ovmf --ostype other \
  --net0 virtio,bridge=vmbr0

# Add EFI disk
qm set 200 --efidisk0 local-lvm:1,efitype=4m,pre-enrolled-keys=0

# Add main disk (VirtIO for Big Sur+, SATA for older)
qm set 200 --virtio0 local-lvm:64,cache=writeback,discard=on,ssd=1

# Enable ISA serial guest agent
qm set 200 --agent enabled=1,type=isa

# Attach OpenCore and installer ISOs
qm set 200 --ide0 local:iso/OpenCore.iso,media=cdrom
qm set 200 --ide2 local:iso/macos-installer.iso,media=cdrom

# Set boot order
qm set 200 --boot order='ide0;virtio0'
```

## Agent Installation

### From a Modern Machine (recommended for old macOS)

Old macOS VMs can't reach GitHub due to TLS incompatibility. Transfer the binary from another machine:

```bash
# On a modern machine — download the binary
curl -L -o mac-guest-agent \
  https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-amd64

# Copy to the VM
scp mac-guest-agent user@<vm-ip>:/tmp/
```

### Inside the macOS VM

```bash
sudo cp /tmp/mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

The `--install` command:
- Copies the LaunchDaemon plist to `/Library/LaunchDaemons/`
- Creates the freeze hooks directory at `/etc/qemu/fsfreeze-hook.d/`
- Installs a default config at `/etc/qemu/qemu-ga.conf.default`
- Sets up log rotation via newsyslog
- Starts the service

### Verify Installation

```bash
# Inside the VM
sudo mac-guest-agent --self-test

# From the PVE host
qm agent 200 ping
qm agent 200 get-osinfo
qm agent 200 network-get-interfaces
```

## Backup Configuration

### Making Backups Consistent

PVE calls `guest-fsfreeze-freeze` before taking a snapshot during backup. The agent responds by:

1. Running freeze hook scripts (database flush, service pause)
2. Creating an APFS snapshot via `tmutil` (10.13+)
3. Calling `sync()` + `F_FULLFSYNC` to flush all data to disk
4. Continuously syncing every 100ms during the freeze window
5. Restricting commands to prevent new disk writes

After the snapshot, PVE calls `guest-fsfreeze-thaw` which reverses everything.

### Check Backup Readiness

```bash
# Inside the VM
sudo mac-guest-agent --self-test
```

Look for:
- **APFS support: yes** — freeze will create a COW snapshot (best consistency)
- **tmutil snapshots: available** — APFS snapshot mechanism is functional
- **hook directory** — hooks are installed and validated
- **Backup readiness: ready** — overall verdict

If APFS is not available (pre-10.13), freeze still works via `sync()` + `F_FULLFSYNC`, which flushes all data to physical media. This is the same level of consistency as most Linux VMs without LVM.

### Freeze Test

Verify freeze/thaw works before relying on it for production backups:

```bash
# From the PVE host
qm guest cmd 200 fsfreeze-freeze
qm guest cmd 200 fsfreeze-status    # Should show "frozen"
# Take a manual snapshot
qm snapshot 200 test-snapshot
qm guest cmd 200 fsfreeze-thaw
qm guest cmd 200 fsfreeze-status    # Should show "thawed"
# Clean up
qm delsnapshot 200 test-snapshot
```

### Freeze Hook Scripts

Drop scripts in `/etc/qemu/fsfreeze-hook.d/` to flush databases before freeze:

```bash
# Example: /etc/qemu/fsfreeze-hook.d/mysql.sh
#!/bin/bash
case "$1" in
    freeze) mysql -u root -e "FLUSH TABLES WITH READ LOCK;" ;;
    thaw)   mysql -u root -e "UNLOCK TABLES;" ;;
esac
```

Requirements for hook scripts:
- Must be owned by root (uid 0)
- Must not be world-writable
- Must be executable
- 30-second timeout per script
- Scripts run alphabetically on freeze, reverse on thaw

See `configs/hooks/` for ready-made hooks for common databases.

## Thin Disk Provisioning

### Enable TRIM (reclaim free space from VM disk)

**PVE host:**
```bash
qm set 200 --virtio0 local-lvm:vm-200-disk-1,discard=on,ssd=1
# Requires VM restart (stop + start, not reboot)
```

**macOS VM (one-time, requires reboot):**
```bash
sudo trimforce enable
```

**Verify:**
```bash
diskutil info disk0 | grep -i "Solid State\|TRIM"
# Should show: Solid State: Yes, TRIM Support: Yes
```

After this, macOS sends TRIM automatically on every file delete. Free space is reclaimed on the PVE host in real-time. The `guest-fstrim` command is a no-op because macOS handles TRIM natively.

### Reclaim Existing Free Space

Space freed before TRIM was enabled needs a one-time manual reclaim:

```bash
# Inside the VM (run during maintenance window)
dd if=/dev/zero of=/tmp/.reclaim bs=4m 2>/dev/null; rm -f /tmp/.reclaim; sync
```

## Security Profiles

### Recommended: PVE Management

Allows standard PVE operations (shutdown, backup freeze, system info) but blocks exec, file I/O, SSH keys, and password changes:

```ini
# /etc/qemu/qemu-ga.conf
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-set-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-fsfreeze-freeze,guest-fsfreeze-thaw,guest-network-get-interfaces,guest-network-get-route,guest-shutdown
```

### Minimal: Monitoring Only

Read-only queries. No modifications, no exec, no freeze:

```ini
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-network-get-interfaces,guest-network-get-route
```

### Full Admin

All commands enabled (default). Equivalent to Linux qemu-ga defaults.

## Host-Side Validation Checklist

Run this from the PVE host after setting up a macOS VM:

```bash
VMID=200

echo "=== PVE macOS VM Validation ==="

# 1. Agent config
echo -n "Agent config: "
grep -q "agent: enabled=1,type=isa" /etc/pve/qemu-server/$VMID.conf && echo "OK (ISA)" || echo "MISSING"

# 2. Disk settings
echo -n "Disk discard: "
grep -q "discard=on" /etc/pve/qemu-server/$VMID.conf && echo "OK" || echo "MISSING"

echo -n "Disk SSD: "
grep -q "ssd=1" /etc/pve/qemu-server/$VMID.conf && echo "OK" || echo "MISSING"

# 3. Agent ping
echo -n "Agent ping: "
qm agent $VMID ping >/dev/null 2>&1 && echo "OK" || echo "FAILED"

# 4. OS info
echo -n "OS info: "
qm agent $VMID get-osinfo 2>/dev/null | grep -q "macOS\|Mac OS" && echo "OK" || echo "FAILED"

# 5. Network
echo -n "Network: "
qm agent $VMID network-get-interfaces 2>/dev/null | grep -q "ip-address" && echo "OK" || echo "FAILED"

# 6. Freeze round-trip
echo -n "Freeze: "
qm guest cmd $VMID fsfreeze-freeze >/dev/null 2>&1 && \
  qm guest cmd $VMID fsfreeze-thaw >/dev/null 2>&1 && echo "OK" || echo "FAILED"

echo "=== Done ==="
```

## Accurate Memory Reporting

Without a guest agent, PVE shows macOS VMs using 100% of allocated RAM because macOS has no balloon driver. With this agent installed, PVE displays actual memory usage from inside the guest.

The agent reports real memory usage via `guest-get-memory-blocks`, using macOS Mach VM statistics (wired + active + compressed pages). PVE reads this data and displays it in the VM summary.

**Before agent:** `mem: 8.00 GiB / maxmem: 8.00 GiB` (always 100%)
**With agent:** `mem: 4.10 GiB / maxmem: 8.00 GiB` (actual usage)

No balloon driver needed. The agent handles it.

**Important:** This is accurate *reporting*, not memory reclamation. There is no balloon driver for macOS — the full allocated RAM remains reserved by the VM on the host regardless of what the guest is actually using. PVE now shows you how much the guest is using, but it cannot reclaim unused guest memory for other VMs. If you need to free host RAM, reduce the VM's memory allocation in PVE.

**Note:** A VM reboot is required after installing the agent for PVE to start showing accurate memory numbers. If you still see 100% after install, reboot the VM.

## PVE Command Limitations

PVE's `qm agent` and `qm guest cmd` only support a hardcoded subset of QGA commands. Newer commands like `guest-network-get-route`, `guest-get-load`, `guest-get-cpustats`, and `guest-get-diskstats` are not in PVE's allowlist yet.

To use these commands, send raw JSON via the QEMU monitor:

```bash
# Via QEMU monitor
qm monitor <vmid> <<< 'guest-network-get-route'

# Or test from inside the VM directly
echo '{"execute":"guest-network-get-route"}' | sudo mac-guest-agent --test
echo '{"execute":"guest-get-load"}' | sudo mac-guest-agent --test
echo '{"execute":"guest-get-cpustats"}' | sudo mac-guest-agent --test
```

All 45 commands work regardless of PVE's allowlist — PVE just can't invoke them through `qm agent` until they update their command list. libvirt's `virsh qemu-agent-command` has no such restriction.

## Troubleshooting

### Agent not responding to `qm agent ping`

1. **Check agent is running in the VM:**
   ```bash
   sudo launchctl list com.macos.guest-agent
   ```

2. **Check serial device exists:**
   ```bash
   ls -la /dev/cu.serial*
   ```
   If no serial device, verify PVE config has `agent: enabled=1,type=isa` and the VM was fully stopped and restarted (not just rebooted).

3. **Check agent log:**
   ```bash
   tail -20 /var/log/mac-guest-agent.log
   ```

4. **Run self-test:**
   ```bash
   sudo mac-guest-agent --self-test
   ```

### Freeze fails or times out

1. **Check hook scripts:**
   ```bash
   sudo mac-guest-agent --self-test  # Reports hook validation
   ls -la /etc/qemu/fsfreeze-hook.d/
   ```

2. **Test freeze manually:**
   ```bash
   # In the VM
   echo '{"execute":"guest-fsfreeze-freeze"}' | sudo mac-guest-agent --test
   echo '{"execute":"guest-fsfreeze-status"}' | sudo mac-guest-agent --test
   echo '{"execute":"guest-fsfreeze-thaw"}' | sudo mac-guest-agent --test
   ```

3. **Check if a hook is hanging:** hooks have a 30-second timeout. Check the log for timeout messages.

### VM shows "QEMU Guest Agent is not running"

This PVE UI message appears when the agent hasn't responded to a ping within the timeout. Common causes:

- Agent not installed (`sudo mac-guest-agent --install`)
- Wrong agent type (`type=isa` required for pre-Big Sur)
- VM needs full stop/start after changing agent config (reboot is not enough)
- Serial device not created (check `ls /dev/cu.serial*` in the VM)

### TRIM not working

1. Verify PVE disk has `discard=on,ssd=1`
2. Verify `trimforce enable` was run in the VM
3. Verify with `diskutil info disk0 | grep TRIM`
4. Note: VM must be fully stopped and restarted after changing disk flags

## Compatibility Quick Reference

| macOS | Binary | Disk | Network | Agent Type | Freeze |
|---|---|---|---|---|---|
| 10.4 Tiger | i386 | SATA | e1000 | type=isa | sync only |
| 10.5–10.6 | i386/x86_64 | SATA | e1000 | type=isa | sync only |
| 10.7–10.12 | x86_64 | SATA | e1000 | type=isa | sync + F_FULLFSYNC |
| 10.13–10.14 | x86_64 | SATA | e1000 | type=isa | sync + APFS snapshot |
| 10.15 | x86_64 | SATA or VirtIO | e1000 or VirtIO | type=isa | sync + APFS snapshot |
| 11.0+ | x86_64 | VirtIO | VirtIO | type=isa or default | sync + APFS snapshot |
