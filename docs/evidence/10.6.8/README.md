# macOS 10.6.8 (Snow Leopard) — evidence

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
cpu: SandyBridge,phys-bits=39
description: 
efidisk0: <redacted>
hostpci0: 0000:0b:00,pcie=1,romfile=vgabios-hd6450.bin,x-vga=1
machine: q35
memory: 8192
name: <redacted>
net0: e1000=<redacted>
numa: 0
ostype: other
sata0: <redacted>,cache=writeback,discard=on,size=60G,ssd=1
sata1: none,media=cdrom
scsihw: virtio-scsi-single
smbios1: uuid=<redacted>
sockets: 1
tags: <redacted>
vga: none
```

### OpenCore specifics

#### General

1. It is pretty much the same as 10.5.8, but it no longer needs patched IONDRVSupport.
2. SSD support is available starting with 10.6, so `ThirdPartyDrives` should be enabled.
3. Preferred `SystemProductName` is `iMac12,1`.

#### GPU pass-through

I have AMD HD 6450 passed-through, thus `hostpci0`.
To have its name properly displayed in system profile one needs.

1. Force-load ATI6000Controller.kext.

      ```xml
      <dict>
        <key>Arch</key>
        <string>Any</string>
        <key>BundlePath</key>
        <string>System/Library/Extensions/ATISupport.kext</string>
        <key>Comment</key>
        <string></string>
        <key>Enabled</key>
        <true/>
        <key>ExecutablePath</key>
        <string>Contents/MacOS/ATISupport</string>
        <key>Identifier</key>
        <string>com.apple.kext.ATISupport</string>
        <key>MaxKernel</key>
        <string>13.99.99</string>
        <key>MinKernel</key>
        <string></string>
        <key>PlistPath</key>
        <string>Contents/Info.plist</string>
      </dict>
      <dict>
        <key>Arch</key>
        <string>Any</string>
        <key>BundlePath</key>
        <string>System/Library/Extensions/ATI6000Controller.kext</string>
        <key>Comment</key>
        <string></string>
        <key>Enabled</key>
        <true/>
        <key>ExecutablePath</key>
        <string>Contents/MacOS/ATI6000Controller</string>
        <key>Identifier</key>
        <string>com.apple.kext.ATI6000Controller</string>
        <key>MaxKernel</key>
        <string>13.99.99</string>
        <key>MinKernel</key>
        <string></string>
        <key>PlistPath</key>
        <string>Contents/Info.plist</string>
      </dict>
      ```

2. Patch ATI6000Controller.kext name resolution logic.

      ```xml
      <dict>
        <key>Arch</key>
        <string>Any</string>
        <key>Base</key>
        <string></string>
        <key>Comment</key>
        <string>Disable ATI 6000 automatic model injection</string>
        <key>Count</key>
        <integer>1</integer>
        <key>Enabled</key>
        <true/>
        <key>Find</key>
        <data>bW9kZWwA</data>
        <key>Identifier</key>
        <string>com.apple.kext.ATI6000Controller</string>
        <key>Limit</key>
        <integer>0</integer>
        <key>Mask</key>
        <data></data>
        <key>MaxKernel</key>
        <string></string>
        <key>MinKernel</key>
        <string></string>
        <key>Replace</key>
        <data>c25hbWUA</data>
        <key>ReplaceMask</key>
        <data></data>
        <key>Skip</key>
        <integer>0</integer>
      </dict>
      ```
