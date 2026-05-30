# libvirt / virt-manager Guide

Complete guide for running macOS VMs with guest agent support on libvirt and virt-manager.

## Domain XML Configuration

### ISA Serial (Required for All macOS Versions)

macOS Big Sur and newer include Apple's own built-in VirtIO guest agent which claims the default VirtIO serial channel. ISA serial is required so our agent gets a dedicated channel.

```xml
<devices>
  <!-- Guest agent via ISA serial (required — VirtIO channel is claimed by Apple's agent) -->
  <serial type='unix'>
    <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
    <target type='isa-serial' port='0'/>
  </serial>
</devices>
```

Inside the VM, the agent finds `/dev/cu.serial1`.

Disk and network devices are separate from the agent transport — use VirtIO for disk/network on Big Sur+, SATA/e1000 on pre-Big Sur. See the examples below.

### Big Sur+ Example (VirtIO disk/network, ISA serial for agent)

```xml
<devices>
  <serial type='unix'>
    <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
    <target type='isa-serial' port='0'/>
  </serial>

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

### Pre-Big Sur Example (SATA disk, e1000 network, ISA serial for agent)

```xml
<devices>
  <serial type='unix'>
    <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
    <target type='isa-serial' port='0'/>
  </serial>

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

## Host-side end-to-end verification (`scripts/verify.sh`)

For a single host-driven verification pass — environment capture, agent communication, freeze/thaw cycles with a content-based behavioural check, and the in-VM `--self-test-json` / `--safe-test-json` diagnostics — use `scripts/verify.sh`:

```bash
curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh | bash -s -- --transport libvirt macos-vm | tee verify.txt
```

How the libvirt transport works:

- **Direct socket I/O, not `virsh qemu-agent-command`.** libvirt's QGA infrastructure only discovers a guest agent from a `<channel type='virtio' target name='org.qemu.guest_agent.0'>` element. On macOS Big Sur+ that VirtIO channel is claimed by Apple's built-in `AppleQEMUGuestAgent` — we use ISA serial precisely to step outside libvirt's QGA convention and avoid the conflict. Which means `virsh qemu-agent-command` cannot reach our agent for our documented configuration. `verify.sh --transport libvirt` therefore **uses `virsh` for discovery only** — it parses `virsh dumpxml` to find the unix socket path bound by the `<serial type='unix'>` element, then talks to that socket directly using the same machinery the UTM and qga-socket transports use.
- **Socket discovery.** Preflight runs `virsh dumpxml <domain>`, walks `<serial type='unix'>` blocks, requires one with a `<target type='isa-serial'>` child (disambiguates our agent from any other unix-typed serial like a console), and extracts the `path` from its `<source>` element. Attribute order is not significant — both `type='isa-serial' port='0'` and `port='0' type='isa-serial'` are accepted (per XML 1.0 / Relax NG, libvirt doesn't constrain attribute order).
- **guest-exec polling.** Once the socket is bound, `verify.sh` issues `guest-exec` directly over the socket, polls `guest-exec-status` at 250ms granularity until `exited == true`, base64-decodes `out-data`/`err-data`, and returns the same envelope shape PVE's `qm guest exec --output-format json` produces. The script's in-VM diagnostics section calls into this primitive without caring which transport is bound.
- **Auto-detection.** With no `--transport` flag, `verify.sh` tries `virsh dominfo <identifier>` and binds the libvirt transport when it exits 0.
- **Privilege.** `virsh dominfo` / `virsh dumpxml` need root or `libvirt`-group membership on the host — the script's preflight runs `virsh list --all` and fails clean with the three standard remediations (run as root, join the `libvirt` group, or set `LIBVIRT_DEFAULT_URI`) if the libvirtd socket isn't reachable. The unix socket itself is created with `mode='bind'` by libvirt when the domain starts and inherits libvirt's filesystem permissions.
- **Configuration prereq.** The agent's [Domain XML Configuration](#domain-xml-configuration) section above adds the documented `<serial type='unix'><source mode='bind' path='...'/><target type='isa-serial' port='0'/></serial>` element. Without it the in-guest agent has nothing to talk to AND preflight cannot discover a socket path — verify.sh fails clean at preflight rather than running an inconsistent check pipeline. Note: this is ISA serial — VirtIO is claimed by Apple's built-in `AppleQEMUGuestAgent` on Big Sur+, so we use ISA instead. See `docs/COMPATIBILITY.md` for the rationale.

`verify.sh --help` lists the rest of the flags (`--no-freeze`, `--no-in-vm`, `--no-env-capture`, `--no-appendix`, `--no-redact`, `--freeze-cycles N`, `--agent-path`, `--log-path`, `--exec-timeout`). PII (IPv4, MAC, supplied identifier) is redacted by default.

## Guest Agent Commands — Direct Socket Access

**`virsh qemu-agent-command` does NOT work with this configuration.** libvirt's QGA API only sees `<channel type='virtio' name='org.qemu.guest_agent.0'>` elements, not `<serial type='isa-serial'>` ones. Calling `virsh qemu-agent-command` against our ISA-configured domain returns `"QEMU guest agent is not configured"`. This is structural — there is no libvirt-side workaround for it. Either:

- **Use direct socket access** (recommended; documented below) — talk to the unix socket on the host filesystem that the `<serial type='unix'>` element binds.
- **Use the `--virtio` install path** if you specifically need libvirt's native QGA API. That path requires SIP disabled, unloads Apple's `AppleQEMUGuestAgent`, and is not a supported configuration; see [`docs/NO_ISA_OVERRIDE.md`](NO_ISA_OVERRIDE.md). After running `--virtio`, `virsh qemu-agent-command` works because the domain XML carries the VirtIO channel libvirt's QGA layer expects.

The rest of this section assumes the supported (ISA serial) path.

### Find the socket path

It's the `<source path='...'/>` from the `<serial type='unix'>` element in your domain XML — exactly what you wrote when you configured the agent. To rediscover it from a running domain:

```bash
virsh dumpxml macos-vm | perl -ne 'print $1 if m{<source[^>]+path=['"'"'"]([^'"'"'"]+)['"'"'"][^>]*/>}'
```

(or just `grep '<source' | grep path=` if you only have one socket-bound element).

### Basic commands

```bash
SOCK=/var/lib/libvirt/qemu/macos-agent.sock

# Ping the agent
echo '{"execute":"guest-ping"}' | socat - UNIX-CONNECT:"$SOCK"

# Get OS info
echo '{"execute":"guest-get-osinfo"}' | socat - UNIX-CONNECT:"$SOCK"

# Get network interfaces (IP addresses)
echo '{"execute":"guest-network-get-interfaces"}' | socat - UNIX-CONNECT:"$SOCK"

# Get system load
echo '{"execute":"guest-get-load"}' | socat - UNIX-CONNECT:"$SOCK"
```

Each invocation opens a fresh connection, sends one QGA JSON frame, reads one response, closes. The agent is line-delimited so any tool that can write a JSON line and read a JSON line works (`socat`, `nc -U`, Python with `socket.AF_UNIX`, Perl with `IO::Socket::UNIX`).

### Shutdown and reboot

```bash
echo '{"execute":"guest-shutdown","arguments":{"mode":"powerdown"}}' | socat - UNIX-CONNECT:"$SOCK"
echo '{"execute":"guest-shutdown","arguments":{"mode":"reboot"}}'    | socat - UNIX-CONNECT:"$SOCK"
echo '{"execute":"guest-shutdown","arguments":{"mode":"halt"}}'      | socat - UNIX-CONNECT:"$SOCK"
```

### Filesystem freeze / thaw

```bash
echo '{"execute":"guest-fsfreeze-freeze"}' | socat - UNIX-CONNECT:"$SOCK"
echo '{"execute":"guest-fsfreeze-status"}' | socat - UNIX-CONNECT:"$SOCK"

# (Take your snapshot here while the filesystem is frozen.)

echo '{"execute":"guest-fsfreeze-thaw"}'   | socat - UNIX-CONNECT:"$SOCK"
```

### Verify freeze support

```bash
echo '{"execute":"guest-info"}' | socat - UNIX-CONNECT:"$SOCK" | python3 -c "
import json, sys
info = json.load(sys.stdin)['return']
cmds = {c['name']: c['enabled'] for c in info['supported_commands']}
print(f'Freeze supported: {cmds.get(\"guest-fsfreeze-freeze\", False)}')
print(f'Agent version:    {info[\"version\"]}')
"
```

## Snapshots — `virsh --quiesce` does NOT work

libvirt's `virsh snapshot-create-as ... --quiesce` flag calls `guest-fsfreeze-freeze` / `guest-fsfreeze-thaw` through libvirt's QGA API. That API can't see our agent (per the section above), so `--quiesce` returns `"QEMU guest agent is not configured"` and the snapshot is taken without filesystem quiesce — i.e., the snapshot may be inconsistent.

The quiesce workflow under our ISA configuration is a three-step sequence the operator drives manually:

```bash
SOCK=/var/lib/libvirt/qemu/macos-agent.sock

# 1. Freeze
echo '{"execute":"guest-fsfreeze-freeze"}' | socat - UNIX-CONNECT:"$SOCK"

# 2. Take the snapshot (no --quiesce — we've already quiesced)
virsh snapshot-create-as macos-vm snap1 --disk-only

# 3. Thaw
echo '{"execute":"guest-fsfreeze-thaw"}' | socat - UNIX-CONNECT:"$SOCK"
```

If a backup tool needs the libvirt `--quiesce` API to work — e.g., a Velero plugin that doesn't expose hooks — your options are (a) wrap the snapshot operation in a script that does the manual freeze sequence before/after, or (b) use the `--virtio` install path so libvirt's QGA layer can reach the agent. Option (a) keeps the supported configuration; option (b) is documented in `docs/NO_ISA_OVERRIDE.md` with its caveats.

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

    <!-- Guest agent via ISA serial (required — VirtIO claimed by Apple's agent) -->
    <serial type='unix'>
      <source mode='bind' path='/var/lib/libvirt/qemu/macos-agent.sock'/>
      <target type='isa-serial' port='0'/>
    </serial>

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

### Agent responds with only 18 commands or missing commands

If `guest-info` shows ~18 commands with Apple-proprietary ones like `apple-guest-set-remote-login`, you're talking to Apple's built-in VirtIO agent, not ours. Switch from `<channel type='virtio'>` to `<serial type='isa-serial'>` in your domain XML. See [Why ISA Serial](PLATFORMS.md#why-isa-serial-not-virtio) for details.

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
