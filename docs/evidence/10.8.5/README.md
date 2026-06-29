# macOS 10.8.5 (Mountain Lion) — evidence

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

1. Very similar to 10.7.5.
2. Preferred `SystemProductName` is `iMac14,2`.
3. Needs `device-id` injection (`0F100000`) via DeviceProperties for e1000 Ethernet to work. gfxutil can be used to get correct device path for a particular VM.
4. Will need NVRAM injection of `4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:background-color:<BFBFBF00>` to avoid boot logo conflicts with OpenCore UI.
5. QEMU Haswell emulation is slightly inaccurate for macOS, specifying `Cpuid1Data` (`C3060300 00000000 00000000 00000000`) / `Cpuid1Mask` (`FFFFFFFF 00000000 00000000 00000000`) may be needed for proper CPU detection.

#### GPU pass-through

AMD HD 6450 does not work out of the box, most likely needs a framebuffer injection.
