# macOS 10.4.11 (Tiger) — evidence

**Contributor:** [@vit9696](https://github.com/vit9696) via [PR #3](https://github.com/mav2287/mac-guest-agent/pull/3).
**Captured against:** mac-guest-agent v2.4.3.
**Result:** 35 passed, 0 failed.

---

### Hardware config

```
agent: enabled=1,type=isa
args: -device usb-kbd -device usb-audio,audiodev=main -audiodev none,id=main
bios: ovmf
boot: order=sata0;sata2
cores: 4
cpu: Penryn
description:
efidisk0: <redacted>
machine: q35
memory: 8192
meta: <redacted>
name: <redacted>
net0: e1000=<redacted>
numa: 0
ostype: other
sata0: <redacted>
sata1: <redacted>
sata2: none,media=cdrom
scsihw: virtio-scsi-single
smbios1: uuid=<redacted>
sockets: 1
tags: <redacted>
vga: vmware
```

### OpenCore specifics

#### `Booter` quirks

```xml
    <key>Quirks</key>
    <dict>
      <key>AllowRelocationBlock</key>
      <true/>
      <key>AvoidRuntimeDefrag</key>
      <true/>
      <key>DevirtualiseMmio</key>
      <false/>
      <key>DisableSingleUser</key>
      <false/>
      <key>DisableVariableWrite</key>
      <false/>
      <key>DiscardHibernateMap</key>
      <false/>
      <key>EnableSafeModeSlide</key>
      <false/>
      <key>EnableWriteUnprotector</key>
      <true/>
      <key>FixupAppleEfiImages</key>
      <true/>
      <key>ForceBooterSignature</key>
      <false/>
      <key>ForceExitBootServices</key>
      <false/>
      <key>ProtectMemoryRegions</key>
      <false/>
      <key>ProtectSecureBoot</key>
      <false/>
      <key>ProtectUefiServices</key>
      <false/>
      <key>ProvideCustomSlide</key>
      <false/>
      <key>ProvideMaxSlide</key>
      <integer>0</integer>
      <key>RebuildAppleMemoryMap</key>
      <true/>
      <key>ResizeAppleGpuBars</key>
      <integer>-1</integer>
      <key>SetupVirtualMap</key>
      <false/>
      <key>SignalAppleOS</key>
      <false/>
      <key>SyncRuntimePermissions</key>
      <false/>
    </dict>
```

#### Kernel patches

```xml
      <dict>
        <key>Arch</key>
        <string>i386</string>
        <key>Base</key>
        <string></string>
        <key>Comment</key>
        <string>Fix Intel e1000 link detection in QEMU</string>
        <key>Count</key>
        <integer>0</integer>
        <key>Enabled</key>
        <true/>
        <key>Find</key>
        <data>x0QkBBEAAAAAAADoAAAAAA==</data>
        <key>Identifier</key>
        <string>com.apple.driver.AppleIntel8254XEthernet</string>
        <key>Limit</key>
        <integer>0</integer>
        <key>Mask</key>
        <data>//////////8AAAD/AAAAAA==</data>
        <key>MaxKernel</key>
        <string>8.99.99</string>
        <key>MinKernel</key>
        <string>8.0.0</string>
        <key>Replace</key>
        <data>i0QkCMcACKwAADHAkJCQkA==</data>
        <key>ReplaceMask</key>
        <data></data>
        <key>Skip</key>
        <integer>0</integer>
      </dict>
```

This patch requires force-loading:

- System/Library/Extensions/IONetworkingFamily.kext/Contents/MacOS/IONetworkingFamily
- System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleIntel8254XEthernet.kext

#### NVRAM variables

```xml
      <key>4D1EDE05-38C7-4A6A-9CC6-4BCCA8B38C14</key>
      <dict>
        <key>DefaultBackgroundColor</key>
        <data>v7+/AA==</data>
      </dict>
```

#### Other

- `DummyPowerManagement` should be enabled.
- Screen resolution can be set in OpenCore config.
- There is no SSD support.
