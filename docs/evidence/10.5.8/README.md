# macOS 10.5.8 (Leopard) — evidence

**Contributor:** [@vit9696](https://github.com/vit9696).
**Captured against:** mac-guest-agent v2.4.3.
**Result:** 35 passed, 0 failed.

---

### Hardware config

```
agent: enabled=1,type=isa
args: -device usb-kbd -device usb-audio,audiodev=main -audiodev none,id=main
bios: ovmf
boot: order=sata0;sata1
cores: 4
cpu: Nehalem
description:
efidisk0: <redacted>
machine: q35
memory: 8192
name: <redacted>
net0: e1000=<redacted>
numa: 0
ostype: other
sata0: <redacted>
sata1: none,media=cdrom
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

#### Kernel extensions

Patched [IONDRVSupport](https://github.com/acidanthera/IONDRVSupport) is needed to support many graphics applications (e.g. third-party VNC servers) due to uninitialised memory bug.

```xml
      <dict>
        <key>Arch</key>
        <string>i386</string>
        <key>BundlePath</key>
        <string>IONDRVSupport.kext</string>
        <key>Comment</key>
        <string>Acidanthera IONDRVSupport</string>
        <key>Enabled</key>
        <true/>
        <key>ExecutablePath</key>
        <string>IONDRVSupport</string>
        <key>MaxKernel</key>
        <string>9.99.99</string>
        <key>MinKernel</key>
        <string>9.0.0</string>
        <key>PlistPath</key>
        <string>Info.plist</string>
      </dict>
```

This kext requires force-loading:

- System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily


#### Other

- `DummyPowerManagement` should be enabled.
- Screen resolution can be set in OpenCore config.
- There is no SSD support.
