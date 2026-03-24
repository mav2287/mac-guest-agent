# Compatibility Matrix

## Evidence Levels

These terms are used consistently throughout all project documentation:

| Term | Definition |
|---|---|
| **Runtime-tested** | Agent launched and exercised on a real installed OS (in a VM or on hardware) |
| **PVE-integrated** | Real guest/host round-trip validated through Proxmox VE (`qm agent <vmid> ping`) |
| **Installer-verified** | Deep static inspection of installer image: kext, C library symbols, frameworks, PCI class match |
| **Best-effort** | Code-level static analysis only. APIs should work based on review, no installer or runtime testing |

## Support Tiers

| Tier | Definition | Required Evidence |
|---|---|---|
| **Tier 1** | Production-ready | Runtime-tested + PVE-integrated (or test-mode equivalent) + freeze verified |
| **Tier 2** | High confidence | Installer-verified (kext, symbols, frameworks, PCI class match all confirmed) |
| **Tier 3** | Theoretical | Best-effort only |

## macOS Version Matrix

| macOS | Tier | Binary | Launches | Self-test | PVE Integration | ISA Serial | Freeze | Evidence |
|---|---|---|---|---|---|---|---|---|
| 10.4 Tiger | 3 | i386 target exists | Untested | Untested | Untested | Untested | Untested | Static analysis only |
| 10.5 Leopard | 3 | — | Untested | Untested | Untested | Untested | Untested | No installer |
| 10.6 Snow Leopard | 3 | x86_64 (deployment target) | Untested | Untested | Untested | Untested | Untested | Binary target verified |
| **10.7 Lion** | **2** | **x86_64 + i386** | Untested | Untested | Untested | **Kext v3.0** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.8 Mountain Lion** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.1** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| 10.9 Mavericks | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer downloading |
| **10.10 Yosemite** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.11 El Capitan** | **1** | **x86_64** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v3.2** | **Yes (HFS+)** | **290/290 stress, mount-verified snapshot. Deep verify 4/4** |
| **10.12 Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.13 High Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.14 Mojave** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.15 Catalina** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **11.6 Big Sur** | **2** | **arm64 + x86_64** | Untested | Untested | Untested | **Kext v3.2 + VirtIO** | Untested | **Deep verify 4/4: kext + symbols (dyld cache) + frameworks + VirtIO v74.120.4** |
| 12.x Monterey | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer downloading |
| 13.x Ventura | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer downloading |
| 14.x Sonoma | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer downloading |
| **26.3 Tahoe** | **1** | **arm64 + x86_64** | **Yes** | **Yes** | **No (test mode only)** | **Kext v3.2** | **Yes (APFS, dry-run)** | **48 unit + 31 proactive + 62 integration tests** |

## Sub-Evidence Matrix

Detailed breakdown of what has been verified per version. All installer-verified versions (Tier 2) pass all four core checks.

| macOS | Serial Driver | C Library Symbols | Frameworks | Required Tools | APFS/VirtIO | Binary Target |
|---|---|---|---|---|---|---|
| 10.7 Lion | v3.0, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.8 Mountain Lion | v3.1, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.10 Yosemite | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.11 El Capitan | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.6 |
| 10.12 Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.6 |
| 10.13 High Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
| 10.14 Mojave | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
| 10.15 Catalina | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
| 11.6 Big Sur | v3.2, PCI 0x0700 | dyld cache | CF + IOKit | — | APFS + VirtIO v74 | arm64 11.0 |
| 26.3 Tahoe | v3.2, PCI 0x0700 | runtime | CF + IOKit | 10/10 | APFS + VirtIO | arm64 11.0 |

Notes:
- "7/10 tools" and "8/10 tools": BaseSystem images don't include osascript, dscl, or tmutil (these are present in the full installed OS)
- "dyld cache": Big Sur+ moved system libraries into a shared cache; symbols are present but not inspectable via nm
- "runtime": Tahoe verified by running the agent directly, not from installer inspection

## Deep Verification Details

Installer images are analyzed by `scripts/verify-installer.sh` which checks:

- **Apple16X50Serial.kext** presence, bundle version, and IOPCIClassMatch (must be `0x07000000&0xFFFF0000` = PCI class 0x0700 Serial Controller, which matches QEMU ISA serial)
- **20 critical C library symbols** in system sub-libraries: `getifaddrs`, `freeifaddrs`, `getutxent`, `endutxent`, `getloadavg`, `getmntinfo`, `getpwnam`, `sysctlbyname`, `gettimeofday`, `settimeofday`, `host_statistics`, `host_statistics64`, `poll`, `strtok_r`, `fcntl`, `sync`, `tcgetattr`, `tcsetattr`, `tcflush`, `tcdrain`
- **CoreFoundation.framework** and **IOKit.framework** presence
- Required tools: `sw_vers`, `diskutil`, `sysctl`, `shutdown`, `launchctl`
- APFS support: `diskutil` APFS references, `tmutil localsnapshot` availability

### Kext Version Timeline

| macOS | Apple16X50Serial.kext | IOSerialFamily.kext | PCI Class Match |
|---|---|---|---|
| 10.7.5 Lion | v3.0 | v10.0.5 | 0x07000000&0xFFFF0000 |
| 10.8.5 Mountain Lion | v3.1 | v10.0.6 | 0x07000000&0xFFFF0000 |
| 10.10.5 Yosemite | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.11.6 El Capitan | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.12.6 Sierra | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.13.6 High Sierra | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.14.6 Mojave | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.15.7 Catalina | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 11.6 Big Sur | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 26.3 Tahoe | v3.2 | v11 | 0x07000000&0xFFFF0000 |

The ISA serial driver has been unchanged across all verified versions — same version, same PCI class match, same IOSerialFamily dependency.

### Binary Evidence

| Binary | Deployment Target | Load Command | Frameworks | Symbols Required |
|---|---|---|---|---|
| x86_64 | 10.6 (Snow Leopard) | LC_VERSION_MIN_MACOSX | CoreFoundation, IOKit, libSystem.B | 121 |
| arm64 | 11.0 (Big Sur) | LC_BUILD_VERSION | CoreFoundation, IOKit, libSystem.B | 122 |

Both binaries link only against system frameworks (CoreFoundation, IOKit) and libSystem.B.dylib. No third-party dependencies. All 20 critical symbols verified present in every installer-verified macOS version.

## Architectural Transitions Covered

| Transition | macOS Version | Impact | Our Handling |
|---|---|---|---|
| HFS+ → APFS | 10.13 High Sierra | Freeze mechanism changes | Runtime detection, APFS snapshot on 10.13+, sync-only fallback |
| No SIP → SIP | 10.11 El Capitan | Kext loading restricted | Not applicable (ISA serial uses built-in kext) |
| Intel → Apple Silicon | 11.0 Big Sur | Architecture change | Separate arm64 binary, universal fat binary |
| No VirtIO → Native VirtIO | 11.0 Big Sur | Communication path | ISA serial primary, VirtIO auto-detected as secondary |
| 32-bit → 64-bit only | 10.6 Snow Leopard | Binary architecture | x86_64 target from 10.6, i386 Makefile target for older |
| bash → zsh default | 10.15 Catalina | Shell for guest-exec | Uses /bin/sh (always available), not login shell |

## Recommended Anchor VMs for Expanded Validation

| macOS | Priority | Why |
|---|---|---|
| 10.7 Lion | Medium | Oldest available installer, pre-SIP |
| 10.13 High Sierra | High | APFS transition — validates freeze snapshot path |
| 11.0 Big Sur | High | Apple Silicon + native VirtIO — validates arm64 + both transports |
| 14.x Sonoma | Medium | Recent Intel/AS — validates current compatibility |

## Verification Workflow

Installer verification is the primary path for expanding this matrix. It proves the OS environment has everything the agent needs without requiring a running VM.

### Step 1: Installer Verification (Best-effort → Tier 2)

```bash
# Text output (human-readable)
./scripts/verify-installer.sh /path/to/Install*.app

# JSON output (machine-readable, for automation)
./scripts/verify-installer.sh --json /path/to/Install*.app > results/10.X.json

# Batch verification
./scripts/verify-all-installers.sh /path/to/installer/directory
```

A version is promoted to **Tier 2** when all four core checks pass:
- Apple16X50Serial.kext present with PCI class 0x0700
- All 20 critical C library symbols present
- CoreFoundation and IOKit frameworks present
- Required tools present (sw_vers, diskutil, sysctl, shutdown, launchctl)

### Step 2: Runtime Validation (Tier 2 → Tier 1)

Boot a VM from the installer on PVE, then:

```bash
# 1. Install the agent
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install

# 2. Self-test (text or JSON)
sudo mac-guest-agent --self-test
sudo mac-guest-agent --self-test-json > selftest-10.X.json

# 3. Safe test suite (read-only, non-destructive)
./tests/safe_test.sh /usr/local/bin/mac-guest-agent

# 4. PVE integration (from the host)
qm agent <vmid> ping
qm agent <vmid> get-osinfo
qm agent <vmid> network-get-interfaces

# 5. Freeze test (from the host, during a backup window)
qm guest cmd <vmid> fsfreeze-freeze
qm guest cmd <vmid> fsfreeze-status
# take snapshot
qm guest cmd <vmid> fsfreeze-thaw
```

A version is promoted to **Tier 1** when all of the above pass.

### Storing Results

Verification JSON outputs should be stored in `results/` (gitignored) during development, and referenced by build number in the Evidence column of the matrix above.

## Quality Metrics

| Metric | Value |
|---|---|
| Static analysis (clang --analyze) | 0 bugs across 21 source files |
| Memory leaks (macOS leaks tool) | 0 leaks, 1143 allocations, 173KB |
| Fuzz testing (ASAN, 210k rounds) | 0 crashes |
| Linux qemu-ga command parity | 44/44 registered. 2 Linux-side commands not implemented: guest-get-devices (Windows-only), guest-network-get-route (QEMU 9.1+, Linux/Windows only) |
| Code coverage | 55.88% line, 80.95% function (remainder requires root, real hardware, or destructive operations) |
| Test suite | 48 unit + 31 proactive + 210k fuzz + 62 integration |
