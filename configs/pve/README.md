# PVE VM Configurations

Ready-to-use Proxmox VE VM configurations for the four anchor VMs. These cover every major architectural boundary in macOS history.

## Anchor VMs

| Config | macOS | Purpose | Disk | Network |
|---|---|---|---|---|
| `tiger-10.4.conf` | 10.4 Tiger | Oldest supported, i386 | SATA | e1000 |
| `high-sierra-10.13.conf` | 10.13 High Sierra | APFS transition, freeze snapshots | SATA + SSD | e1000 |
| `big-sur-11.conf` | 11 Big Sur | VirtIO + modern stack | VirtIO | VirtIO |
| `sequoia-15.conf` | 15 Sequoia | Current stable release | VirtIO | VirtIO |

## Usage

1. Copy the desired `.conf` to your PVE host:
   ```bash
   scp configs/pve/high-sierra-10.13.conf root@pve:/etc/pve/qemu-server/200.conf
   ```

2. Edit the file to replace:
   - `VMID` with your actual VM ID
   - `XX:XX:XX:XX:XX:XX` with a generated MAC (or remove to auto-generate)
   - `local-lvm` / `local:iso` with your actual storage names
   - ISO filenames with your actual installer ISOs

3. Create the required disks:
   ```bash
   qm create 200
   # Then start and install macOS via OpenCore
   ```

## Requirements

- [OpenCore ISO](https://github.com/LongQT-sea/OpenCore-ISO) configured for the target macOS version
- macOS installer ISO (create with `scripts/create-install-iso.sh`)
- PVE 7.x or later

## Hardware Notes

| macOS Version | Disk | Network | CPU | Why |
|---|---|---|---|---|
| 10.4–10.14 | **SATA only** | **e1000** | Nehalem | No VirtIO driver support |
| 10.15 Catalina | SATA or VirtIO | e1000 or VirtIO | Nehalem | VirtIO may work but untested |
| 11.0+ Big Sur | **VirtIO** | **VirtIO** | Nehalem | Native VirtIO driver (AppleVirtIO.kext) |

## Test Sequence

After installing macOS and the guest agent:

```bash
# Inside the VM:
sudo mac-guest-agent --self-test
sudo mac-guest-agent --self-test-json > /tmp/selftest.json

# From the PVE host:
qm agent <vmid> ping
qm agent <vmid> get-osinfo
qm agent <vmid> network-get-interfaces
qm agent <vmid> network-get-route

# Freeze test (during maintenance window):
qm guest cmd <vmid> fsfreeze-freeze
qm guest cmd <vmid> fsfreeze-status
# Take snapshot
qm guest cmd <vmid> fsfreeze-thaw
```
