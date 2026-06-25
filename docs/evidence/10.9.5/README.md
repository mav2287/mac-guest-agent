# macOS 10.9.5 (Mavericks) — evidence

**Contributor:** [@vit9696](https://github.com/vit9696).
**Captured against:** mac-guest-agent v2.5.6.
**Result:** 35 passed, 0 failed.

---

### Hardware config

```
agent: enabled=1,type=isa
args: -device usb-kbd -device usb-audio,audiodev=main -audiodev none,id=main -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
bios: ovmf
boot: order=sata0;sata1
cores: 4
cpu: Haswell-noTSX,phys-bits=39
description:
efidisk0: <redacted>
machine: q35
memory: 8192
name: <redacted>
net0: e1000=<redacted>
numa: 0
ostype: other
sata0: <redacted>,cache=writeback,discard=on,size=60G,ssd=1
sata1: none,media=cdrom
scsihw: virtio-scsi-single
smbios1: uuid=<redacted>,serial=<redacted>,base64=1
sockets: 1
tags: <redacted>
vga: vmware
```

### OpenCore specifics

#### General

1. Exactly the same as with 10.8.5.

