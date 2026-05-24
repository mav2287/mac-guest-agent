# macOS 10.4.11 (Tiger) — evidence

**Contributor:** [@vit9696](https://github.com/vit9696) via [PR #3](https://github.com/mav2287/mac-guest-agent/pull/3).
**Captured against:** mac-guest-agent v2.4.2, legacy `scripts/pve-verify.sh` (now `scripts/verify.sh` in v2.4.3+).
**Result:** 11 passed, 1 failed.

> **The single FAIL** — `frozen state — agent answered get-osinfo while frozen (not genuinely frozen)` — was a **bug in the verifier script, not in the agent.** `pve-verify.sh` checked the `qm agent` process's exit code to decide whether the agent had genuinely rejected the freeze-gated command. PVE's `register_command` dispatcher wraps QGA error envelopes as `{result:{error:{...}}}` and exits 0 regardless, so the exit-code check could never distinguish honest rejection from a silently-served reply. The agent on this 10.4.11 setup was correctly rejecting `get-osinfo` during the freeze window (per `src/agent.c:73`) — the script was lying about what it saw.
>
> Vit9696's report of this FAIL is what drove the three-phase rewrite that became Phase 1–4 of `docs/PLAN.md` and v2.4.3. The fix landed in commit `d59f8fb` (audit finding 5): the new `scripts/verify.sh` inspects response content (`"pretty-name"` → FAIL, `"Command not allowed while filesystem is frozen"` → PASS) rather than exit code.
>
> Files in this directory use the legacy 3-file layout (`pve-verify.txt` + `selftest.json` + `safetest.json`) — still accepted per `docs/evidence/README.md`. A re-run against v2.4.3 + `scripts/verify.sh` would produce the schema-2.0 single-file `verify.txt` + `verify.json` layout and is welcome but not required; the bug this evidence helped uncover is already fixed.

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
