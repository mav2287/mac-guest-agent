# Platform Setup

The agent works with any QEMU-based hypervisor. It auto-detects the serial transport (VirtIO or ISA) and connects to whichever is available.

For **Proxmox VE**, see the dedicated [PVE Guide](PVE.md).

## Plain QEMU

### ISA Serial (all macOS versions, 10.4+)

```bash
qemu-system-x86_64 \
  -device isa-serial,chardev=agent \
  -chardev socket,id=agent,path=/tmp/qga.sock,server=on,wait=off \
  # ... other VM options
```

Inside the VM, the agent finds `/dev/cu.serial1`.

### VirtIO Serial (Big Sur 11.0+ only)

```bash
qemu-system-x86_64 \
  -device virtio-serial \
  -device virtserialport,chardev=agent,name=org.qemu.guest_agent.0 \
  -chardev socket,id=agent,path=/tmp/qga.sock,server=on,wait=off \
  # ... other VM options
```

Inside the VM, the agent finds `/dev/cu.org.qemu.guest_agent.0`.

### Sending commands from the host

```bash
# Via the socket directly
echo '{"execute":"guest-ping"}' | socat - UNIX-CONNECT:/tmp/qga.sock

# Or use qemu-ga-client if available
```

## libvirt / virt-manager

### VirtIO channel (Big Sur+ only)

Add to domain XML:

```xml
<channel type='unix'>
  <source mode='bind' path='/var/lib/libvirt/qemu/guest-agent.sock'/>
  <target type='virtio' name='org.qemu.guest_agent.0'/>
</channel>
```

### ISA serial channel (all macOS versions)

```xml
<serial type='unix'>
  <source mode='bind' path='/var/lib/libvirt/qemu/guest-agent.sock'/>
  <target port='0'/>
</serial>
```

### Sending commands

```bash
virsh qemu-agent-command <domain> '{"execute":"guest-ping"}'
virsh qemu-agent-command <domain> '{"execute":"guest-get-osinfo"}'
```

## UTM (macOS host)

UTM uses Apple's Virtualization.framework with VirtIO serial. The agent auto-detects UTM's device at `/dev/cu.virtio`.

1. In UTM VM settings, add a **Serial** device (VirtIO type)
2. Install the agent binary in the VM
3. The agent auto-detects and connects

UTM supports guest agent commands via its scripting interface:
```bash
utmctl exec <vm-name> -- /bin/echo hello
utmctl ip-address <vm-name>
```

## Transport Priority

The agent checks devices in this order (first found wins):

1. **VirtIO serial** — QEMU/PVE/libvirt (`/dev/cu.org.qemu.guest_agent.0`, etc.)
2. **UTM VirtIO** — Apple Virtualization.framework (`/dev/cu.virtio`)
3. **ISA serial** — Universal fallback (`/dev/cu.serial1`, `/dev/cu.serial2`)

On Big Sur+ where both VirtIO and ISA serial could exist, VirtIO is preferred (native driver, better integration). Override with `-p /dev/cu.serial1` to force ISA serial.
