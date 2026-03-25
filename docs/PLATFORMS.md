# Supported Platforms

The agent works with any QEMU-based hypervisor. It auto-detects the serial transport (VirtIO or ISA) and connects to whichever is available.

| Platform | Guide | Transport | Status |
|---|---|---|---|
| **Proxmox VE** | [docs/PVE.md](PVE.md) | ISA serial or VirtIO | First-class, operationally complete |
| **libvirt / virt-manager** | [docs/LIBVIRT.md](LIBVIRT.md) | ISA serial or VirtIO channel | First-class, with domain XML and virsh examples |
| **UTM** | [docs/UTM.md](UTM.md) | VirtIO (Virtualization.framework) | First-class, with utmctl integration |
| **Plain QEMU** | Below | ISA serial or VirtIO | Documented inline |

## Plain QEMU

### ISA Serial (all macOS 10.4+)

```bash
qemu-system-x86_64 \
  -device isa-serial,chardev=agent \
  -chardev socket,id=agent,path=/tmp/qga.sock,server=on,wait=off \
  # ... other VM options
```

The agent finds `/dev/cu.serial1` inside the VM.

### VirtIO Serial (Big Sur 11.0+)

```bash
qemu-system-x86_64 \
  -device virtio-serial \
  -device virtserialport,chardev=agent,name=org.qemu.guest_agent.0 \
  -chardev socket,id=agent,path=/tmp/qga.sock,server=on,wait=off \
  # ... other VM options
```

The agent finds `/dev/cu.org.qemu.guest_agent.0` inside the VM.

### Sending Commands

```bash
# Via the socket
echo '{"execute":"guest-ping"}' | socat - UNIX-CONNECT:/tmp/qga.sock

# Or with qemu-ga-client
qemu-ga-client --address=/tmp/qga.sock ping
```

## Transport Priority

The agent checks devices in this order (first found wins):

1. **VirtIO serial** — PVE, QEMU, libvirt (`/dev/cu.org.qemu.guest_agent.0`)
2. **UTM VirtIO** — Apple Virtualization.framework (`/dev/cu.virtio`)
3. **ISA serial** — Universal fallback (`/dev/cu.serial1`)

On Big Sur+ where both transports could exist, VirtIO is preferred. Override with `-p /dev/cu.serial1` to force ISA.
