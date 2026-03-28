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

## Apple's Built-in VirtIO Agent

Starting with macOS Big Sur (11.0), Apple ships a built-in guest agent as part of `AppleVirtIO.kext`. This agent claims the default VirtIO serial channel (`org.qemu.guest_agent.0`) and speaks a subset of the QGA protocol.

**Confirmed version:** `1.4-AppleVirtIO-230.100.3~3768` (observed on Sequoia 15.7.5, PR #1)

### Apple's Agent Commands (18 total)

| Command | Category |
|---|---|
| `guest-ping` | Protocol |
| `guest-sync` | Protocol |
| `guest-sync-delimited` | Protocol |
| `guest-info` | Protocol |
| `guest-get-time` | System |
| `guest-set-time` | System |
| `guest-network-get-interfaces` | Network |
| `guest-shutdown` | Power |
| `guest-exec` | Exec |
| `guest-exec-status` | Exec |
| `guest-file-open` | File I/O |
| `guest-file-close` | File I/O |
| `guest-file-read` | File I/O |
| `guest-file-write` | File I/O |
| `guest-file-seek` | File I/O |
| `guest-file-flush` | File I/O |
| `apple-guest-set-remote-login` | Apple proprietary |
| `apple-guest-signal-pid` | Apple proprietary |

### Comparison

| Capability | Apple VirtIO Agent | mac-guest-agent |
|---|---|---|
| Transport | VirtIO serial (default) | ISA serial (`type=isa`) |
| Total commands | 18 | 45 |
| Filesystem freeze/thaw | **No** | Yes (APFS snapshot + sync + hooks) |
| Memory reporting | **No** | Yes (real usage via Mach VM stats) |
| OS info (guest-get-osinfo) | **No** | Yes |
| Hostname | **No** | Yes |
| Timezone | **No** | Yes |
| Users | **No** | Yes |
| System load | **No** | Yes |
| CPU stats | **No** | Yes |
| Memory blocks | **No** | Yes |
| Disk info | **No** | Yes |
| Filesystem info | **No** | Yes |
| Routing table | **No** | Yes |
| SSH key management | **No** | Yes |
| User password | **No** | Yes |
| Suspend/hibernate | **No** | Yes |
| TRIM | **No** | Yes (no-op, macOS native) |
| Freeze hooks (DB flush) | **No** | Yes (MySQL, PostgreSQL, Redis) |
| Self-test / diagnostics | **No** | Yes (text + JSON) |
| Backup readiness check | **No** | Yes |
| Security profiles | **No** | Yes (allowlist/blocklist) |
| macOS versions | Big Sur 11.0+ only | 10.4 Tiger through Tahoe |
| File I/O | Yes | Yes |
| Exec | Yes | Yes |
| Shutdown | Yes | Yes |
| Network interfaces | Yes | Yes |
| Time get/set | Yes | Yes |

### Coexistence

Both agents can run simultaneously — Apple's on VirtIO, ours on ISA serial. They do not conflict. PVE communicates with whichever agent is on the configured channel:

- `agent: enabled=1` → talks to Apple's agent (18 commands)
- `agent: enabled=1,type=isa` → talks to our agent (45 commands)

### Why Not Just Use Apple's Agent?

Apple's agent lacks the commands PVE needs for operational management:
- **No `guest-get-osinfo`** — PVE can't identify the guest OS
- **No `guest-fsfreeze-*`** — PVE can't create consistent backups
- **No `guest-get-memory-blocks`** — PVE shows 100% RAM usage
- **No `guest-get-vcpus`** — PVE can't report CPU topology
- **No `guest-get-fsinfo`** — PVE can't show disk usage
- **No SSH key management** — can't inject keys for automation
- **No suspend/hibernate** — only shutdown

For basic use (ping, shutdown, file transfer, exec), Apple's agent works. For production VM management with backup consistency, monitoring, and automation, our agent is required.
