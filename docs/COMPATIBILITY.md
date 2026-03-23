# Compatibility Matrix

## Support Tiers

| Tier | Definition |
|---|---|
| **Tier 1: Validated** | Runtime-tested with PVE integration, freeze verified, stress tested |
| **Tier 2: Build-tested** | Binary compiles for this target, deployment target verified, runtime not tested |
| **Tier 3: Best effort** | APIs should work based on static analysis, no runtime or build testing |

## macOS Version Matrix

| macOS | Tier | Binary | Launches | Self-test | PVE Integration | ISA Serial | Freeze | Evidence |
|---|---|---|---|---|---|---|---|---|
| 10.4 Tiger | 3 | i386 target exists | Untested | Untested | Untested | Untested | Untested | Static analysis only |
| 10.5 Leopard | 3 | — | Untested | Untested | Untested | Untested | Untested | No installer |
| 10.6 Snow Leopard | 2 | x86_64 (deployment target) | Untested | Untested | Untested | Untested | Untested | Binary target verified |
| 10.7 Lion | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 10.8 Mountain Lion | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 10.9 Mavericks | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 10.10 Yosemite | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| **10.11 El Capitan** | **1** | **x86_64** | **Yes** | **Pending** | **Yes (PVE)** | **Yes** | **Yes (HFS+)** | **290/290 stress, mount-verified snapshot** |
| 10.12 Sierra | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 10.13 High Sierra | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available, APFS transition |
| 10.14 Mojave | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 10.15 Catalina | 3 | x86_64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 11.0 Big Sur | 2 | arm64 (deployment target) | Untested | Untested | Untested | Untested | Untested | Installer available, VirtIO native |
| 12.x Monterey | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 13.x Ventura | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| 14.x Sonoma | 3 | arm64 | Untested | Untested | Untested | Untested | Untested | Installer available |
| **26.3 Tahoe** | **1** | **arm64 + x86_64** | **Yes** | **Pending** | **No (test mode only)** | **N/A** | **Yes (APFS, dry-run)** | **48 unit + 31 proactive + 61 integration tests** |

## Architectural Transitions Covered

| Transition | macOS Version | Impact | Our Handling |
|---|---|---|---|
| HFS+ → APFS | 10.13 High Sierra | Freeze mechanism changes | Runtime detection, APFS snapshot on 10.13+, sync-only fallback |
| No SIP → SIP | 10.11 El Capitan | Kext loading restricted | Not applicable (ISA serial needs no kext) |
| Intel → Apple Silicon | 11.0 Big Sur | Architecture change | Separate arm64 binary, universal fat binary |
| No VirtIO → Native VirtIO | 11.0 Big Sur | Communication path | ISA serial primary, VirtIO auto-detected as secondary |
| 32-bit → 64-bit only | 10.6 Snow Leopard | Binary architecture | x86_64 target from 10.6, i386 Makefile target for older |

## Recommended Anchor VMs for Expanded Validation

| macOS | Priority | Why |
|---|---|---|
| 10.7 Lion | Medium | Oldest available installer, pre-SIP |
| 10.13 High Sierra | High | APFS transition — validates freeze snapshot path |
| 11.0 Big Sur | High | Apple Silicon + native VirtIO — validates arm64 + both transports |
| 14.x Sonoma | Medium | Recent Intel/AS — validates current compatibility |

## How to Expand This Matrix

1. Boot anchor VM from installer
2. Install agent binary
3. Run `mac-guest-agent --self-test` (when implemented)
4. Run safe test suite: `./tests/safe_test.sh /path/to/binary`
5. Test PVE integration: `qm agent <vmid> ping`
6. Test freeze: `qm guest cmd <vmid> fsfreeze-freeze` + snapshot + thaw
7. Record results here
