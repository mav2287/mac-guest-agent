# Supported Platforms

The agent works with any QEMU-based hypervisor via ISA serial (`type=isa`).

| Platform | Guide | Transport | Status |
|---|---|---|---|
| **Proxmox VE** | [docs/PVE.md](PVE.md) | ISA serial (`type=isa`) | First-class, operationally complete |
| **libvirt / virt-manager** | [docs/LIBVIRT.md](LIBVIRT.md) | ISA serial | First-class, with domain XML and virsh examples |
| **UTM** | [docs/UTM.md](UTM.md) | See UTM guide | With utmctl integration |
| **Plain QEMU** | Below | ISA serial | Documented inline |

## Why ISA Serial (Not VirtIO)

macOS Big Sur and newer ship with Apple's own built-in VirtIO guest agent (`AppleVirtIO`, ~18 commands). When the default VirtIO serial channel is configured, Apple's agent claims it — not ours. Apple's agent lacks freeze support, memory reporting, routing, and most of the 45 commands we provide.

Using `type=isa` (ISA serial) creates a separate channel via `Apple16X50Serial.kext` that Apple's agent does not claim. This is required on **all** macOS versions for our agent to work.

## Plain QEMU

### ISA Serial (required)

```bash
qemu-system-x86_64 \
  -device isa-serial,chardev=agent \
  -chardev socket,id=agent,path=/tmp/qga.sock,server=on,wait=off \
  # ... other VM options
```

The agent finds `/dev/cu.serial1` inside the VM.

### Sending Commands

```bash
# Via the socket
echo '{"execute":"guest-ping"}' | socat - UNIX-CONNECT:/tmp/qga.sock

# Or with qemu-ga-client
qemu-ga-client --address=/tmp/qga.sock ping
```

## Apple's Built-in VirtIO Agent vs This Agent

| | Apple VirtIO Agent | mac-guest-agent |
|---|---|---|
| Transport | VirtIO serial (default) | ISA serial (`type=isa`) |
| Commands | 18 | 45 |
| Filesystem freeze | No | Yes (APFS snapshot + sync) |
| Memory reporting | No | Yes (real usage via Mach VM stats) |
| Routing table | No | Yes |
| SSH key management | No | Yes |
| File I/O | Yes | Yes |
| Exec | Yes | Yes |
| macOS versions | Big Sur+ only | 10.4 Tiger through Tahoe |
| Freeze hooks | No | Yes (MySQL, PostgreSQL, Redis, custom) |
| Self-test | No | Yes |

Both can coexist — Apple's agent on VirtIO, ours on ISA serial. They do not conflict.
