# macOS 13.7.8 (Ventura) — evidence

**Contributor:** [@vit9696](https://github.com/vit9696).
**Captured against:** mac-guest-agent v2.5.6.
**Result:** 38 passed, 0 failed.

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
machine: q35
memory: 16384
name: <redacted>
net0: virtio=<redacted>
numa: 0
ostype: other
sata0: <redacted>,cache=writeback,discard=on,size=100G,ssd=1
sata1: none,media=cdrom
scsihw: virtio-scsi-single
smbios1: uuid=<redacted>,serial=<redacted>,base64=1
sockets: 1
tags: <redacted>
hostpci0: 0000:0b:00,pcie=1,romfile=vgabios-hd6450.bin,x-vga=1
vga: none
```

### OpenCore specifics

#### General

1. Exactly the same as with 10.14.6.
