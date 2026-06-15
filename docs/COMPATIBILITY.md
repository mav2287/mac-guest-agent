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
| **Tier 1†** | Production-ready, current-artifact retest pending | Runtime-tested against a prior release; the binary recipe is structurally equivalent in the current release (verified by `scripts/verify-legacy-slices.sh` every CI build), but a current-release runtime drop has not yet been captured. A retest closes the dagger. |
| **Tier 2** | High confidence | Installer-verified (kext, symbols, frameworks, PCI class match all confirmed) |
| **Tier 3** | Theoretical | Best-effort only |

## ISA Serial Transport — Why

macOS can be a guest in two distinct virtualisation environments, and they treat the QGA transport differently:

| Host class | Examples | Apple's `AppleQEMUGuestAgent` behaviour | VirtIO channel availability |
|---|---|---|---|
| **Apple Virtualization.framework** | UTM (VZ backend), `vz_run`, anything backed by `VZVirtualMachine` | IOKit-launched on-demand once the `AppleVirtIOAgentDevice` property is matched by the `applevirtio.console` driver | Claimed by Apple's agent |
| **Plain QEMU/KVM** | Proxmox VE, libvirt, raw QEMU (typically with OpenCore for macOS guests) | Never launches — the `applevirtio.console` driver does not load on QEMU, so `AppleVirtIOAgentDevice` is never set and the IOKit match never fires | Technically free |

The historical "use ISA because Apple claims VirtIO" framing is correct for the first row and oversimplified for the second. On Proxmox/QEMU/OpenCore guests, Apple's agent isn't running and the VirtIO console channel would in fact be available. v2.4.x kept VirtIO entries in the channel auto-detect list as a fallback for this case (UTM Emulate backend defaults, custom QEMU command lines without `-device isa-serial`). **As of v2.5.0, the agent supports ISA serial only — the VirtIO fallback was removed.** Three reasons:

1. **One transport across both host classes.** Detecting which environment we're in at runtime would require IOKit introspection and would diverge the launchd plist between VZ and QEMU installs. Using ISA everywhere keeps the install, the launchd config, and the channel-detection list (`src/channel.c`) identical on every macOS guest.
2. **No silent failure on VZ if a disk image moves.** A VirtIO-based install on a QEMU host that gets migrated to UTM Virtualize (or vice versa) would, in the old fallback model, silently route to Apple's 18-command agent the moment Apple's daemon claimed the VirtIO console. The operator might not notice until a backup-time freeze failed. ISA-only eliminates the failure mode entirely.
3. **Loud over silent.** When VirtIO IS the only thing present (legacy UTM Emulate Serial, ported v2.4.x setup), v2.5.0 logs a clear error pointing at the migration path rather than picking VirtIO and hoping for the best. See `src/channel.c log_virtio_diagnostic_if_present()`.

The cost is requiring users to present an ISA UART to the guest. `Apple16X50Serial.kext` has been present on every macOS from 10.4 Tiger (2005) through 26.3 Tahoe (2026) with an identical PCI class match — see [Kext Version Timeline](#kext-version-timeline) below — so this cost is essentially zero on the kernel side; the operator just needs to configure the hypervisor to expose ISA (`type=isa` on PVE, isa-serial device on libvirt, QemuGuestAgent interface on UTM, `-device isa-serial` on raw QEMU). Apple's own `AppleQEMUGuestAgent`, where it does launch, also speaks the QGA protocol but ships only 18 commands and no filesystem freeze (per local inspection on macOS 26.5; see `docs/research/UPSTREAM_NOTES.md` Target 6 for the symbol survey). That feature gap, not the transport question, is the primary reason this project exists.

## macOS Version Matrix

| macOS | Tier | Binary | Launches | Self-test | PVE Integration | ISA Serial | Freeze | Evidence |
|---|---|---|---|---|---|---|---|---|
| **10.4 Tiger** | **1** | **x86_64 (selected)** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v1.9** | **Yes (HFS+)** | **v2.5.4 runtime verified on PVE 9.1.1 Tiger 10.4.11 VM 111 (Penryn / MacPro2,1 SMBIOS via OpenCore). XNU `grade_binary` picks the x86_64 slice on 10.4.7+ even on the i386 kernel (bug-for-feature: x86_64 userland enabled on EM64T host) — drove issue #9 fix (LC_SYMTAB classic binding + weak CF/IOKit + `-mmacosx-version-min=10.4`). 19h 6m continuous stable run with 17,368 messages, zero wedges, zero EOF events, zero EIO reconnects (the v2.5.4 `read==0 → EAGAIN` root-cause fix for issue #10 holds). Full 23-command QGA sweep, file CRUD, SSH key CRUD, exec status polling, ENOEXEC fallback, signaled child (`signal: 15`), `guest-fsfreeze-freeze-list`, frozen-state command gate all PASS. Hook script `/etc/qemu/fsfreeze-hook.d/99-test.sh` fires on freeze+thaw with `$1` correctly set to `"freeze"` / `"thaw"`. **Tiger-specific behavior (updated v2.5.5):** `guest-network-get-interfaces` and `guest-network-get-route` now return real data on Tiger 10.4 — the v2.5.4 empty-array caveat no longer applies. The daemon installs as the i386 slice on Tiger, where native `getifaddrs(3)` returns instantly under launchd (the x86_64-slice hang that motivated the empty-array shim is gone); routes/stats come from `netstat`. Confirmed under a launchd daemon on the real i386 iMac and the QEMU i386 VM — see [`evidence/v2.5.5/`](evidence/v2.5.5/). (The runtime metrics in this row are the v2.5.4 drop and predate the network fix.) 100 consecutive `qm agent ping`s succeed 100/100. 100 freeze/thaw cycles in 268 s, zero state leak. 12-command mixed sequence including BOTH network commands: 12/12 PASS. See [`evidence/10.4.11/`](evidence/10.4.11/) for v2.4.3 baseline (vit9696 PR #5) + v2.5.4 evidence dump. |
| **10.5 Leopard** | **1** | **i386 (selected)** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v1.9** | **Yes (HFS+)** | **v2.5.4 runtime verified on PVE 9.1.1 Leopard 10.5.8 VM 112 (Penryn / MacBookPro3,1 SMBIOS via OpenCore + HfsPlusLegacy.efi + FixupAppleEfiImages). XNU `grade_binary` correctly picks the i386 slice (32-bit kernel + non-Xserve SMBIOS, no x86_64 userland support). **New v2.5.4 fix:** the i386 slice's `guest-exec` failed silently with exit 127 for every binary due to a bug in Apple's pre-10.6 i386 libc `execvp()` wrapper. Codex-assisted root-cause; fixed in `src/cmd-exec.c::exec_child_image()` by calling `execv()` directly for `/`-containing paths (bypassing the wrapper). After patch: full 23-command QGA sweep + file CRUD + SSH key CRUD + exec status polling + ENOEXEC fallback + signaled child + selective freeze + frozen-state command gate all PASS. Self-test 19 PASS / 1 WARN (tmutil absent — expected, tmutil is 10.7+). See [`evidence/10.5.8/`](evidence/10.5.8/) for v2.4.3 baseline (vit9696 PR #5) + v2.5.4 evidence dump. |
| **10.6 Snow Leopard** | **1** | **x86_64 (selected)** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v3.0** | **Yes (HFS+)** | **v2.5.4 runtime verified on PVE 9.1.1 Snow Leopard 10.6.8 build 10K549 VM 113 (Penryn / MacPro3,1 SMBIOS via OpenCore + HfsPlusLegacy.efi). 64-bit kernel `RELEASE_X86_64` xnu-1504.15.3. `grade_binary` correctly picks x86_64 slice. Full 23-command QGA sweep + file CRUD + SSH key CRUD + exec status polling + ENOEXEC fallback + signaled child + selective freeze + frozen-state command gate + hook scripts all PASS. Self-test clean. 30 consecutive `qm agent ping`s pass 30/30 with no wedge or recovery needed. First runtime validation of the 10.6 x86_64 slice path (deployment target for the legacy `LC_UNIXTHREAD`-built x86_64 slice — the slice originally broken in v2.4.3 / issue #4, refixed in v2.5.0). |
| **10.7 Lion** | **2** | **x86_64 + i386** | Untested | Untested | Untested | **Kext v3.0** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700. v2.5.0 universal binary passes static legacy-slice validation; runtime verification pending.** |
| **10.8 Mountain Lion** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.1** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.9 Mavericks** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.10 Yosemite** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.11 El Capitan** | **1** | **x86_64** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v3.2** | **Yes (HFS+)** | **v2.5.4 runtime verified on PVE 9.1.1 BAM-Xserve VM 107 (real-world stripped Xserve3,1 install, MacBookPro3,1 SMBIOS via OpenCore). Full 23-command QGA sweep + file CRUD + SSH key CRUD + exec status polling + ENOEXEC fallback + signaled child + selective freeze + frozen-state command gate + hook scripts all PASS. 30/30 stress pings (~1.24 s each). Self-test 20/0/0. v2.5.2 verifier 38/0 on real Xserve3,1 metal (vit9696, [`evidence/10.11.6/`](evidence/10.11.6/)) — fourth consecutive v2.5.x release confirmed structurally identical on this hardware (v2.5.0 → v2.5.1 → v2.5.2 → v2.5.4). 290/290 stress, mount-verified snapshot. Deep verify 4/4.** |
| **10.12 Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.13 High Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.14 Mojave** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.15 Catalina** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **11.6 Big Sur** | **2** | **arm64 + x86_64** | Untested | Untested | Untested | **Kext v3.2 + VirtIO** | Untested | **Deep verify 4/4: kext + symbols (dyld cache) + frameworks + VirtIO v74.120.4** |
| 12.6 Monterey | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| 13.0 Ventura | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| 14.0 Sonoma | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| **15.7 Sequoia** | **1** | **arm64 + x86_64** | **Yes** | **Yes** | **Yes (PVE)** | **ISA serial** | Untested | **External user confirmed on 15.7.5: self-test pass, PVE integration, VirtIO disk + ISA serial (pgcudahy, PR #1)** |
| **26.3 Tahoe** | **1** | **arm64 + x86_64** | **Yes** | **Yes** | **No (test mode only)** | **Kext v3.2** | **Yes (APFS, dry-run)** | **Full test suite passes (unit + proactive + fuzz + integration + verify-transports + install-flags)** |

## Sub-Evidence Matrix

Detailed breakdown of what has been verified per version. All installer-verified versions (Tier 2) pass all four core checks.

| macOS | Serial Driver | C Library Symbols | Frameworks | Required Tools | APFS/VirtIO | Binary Target |
|---|---|---|---|---|---|---|
| 10.4.11 Tiger | v1.9, PCI 0x0700 | 19/19 required (no host_statistics64) | CF + IOKit | in pkg | — | i386 |
| 10.5 Leopard | v1.9, PCI 0x0700 | in libc.dylib | CF + IOKit | 7/10 | — | i386 |
| 10.6 Snow Leopard | v3.0, PCI 0x0700 | in libSystem.B | CF + IOKit | 7/10 | — | x86_64 10.4 |
| 10.7 Lion | v3.0, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.4 |
| 10.8 Mountain Lion | v3.1, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.4 |
| 10.9 Mavericks | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.4 |
| 10.10 Yosemite | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.4 |
| 10.11 El Capitan | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.4 |
| 10.12 Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.4 |
| 10.13 High Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.4 |
| 10.14 Mojave | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.4 |
| 10.15 Catalina | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.4 |
| 11.6 Big Sur | v3.2, PCI 0x0700 | dyld cache | CF + IOKit | — | APFS + VirtIO v74 | arm64 11.0 |
| 26.3 Tahoe | v3.2, PCI 0x0700 | runtime | CF + IOKit | 10/10 | APFS + VirtIO | arm64 11.0 |

Notes:
- "in Essentials.pkg": Tiger's DVD base image doesn't include the kext, but the full OS install (Essentials.pkg) does
- "no host_statistics64": Tiger's libSystem.B.dylib does not export `host_statistics64` (introduced in 10.6). The symbol is weak-imported by the agent and the `vm_stat` text fallback handles memory stats on 10.4.
- "in libc.dylib" / "in libSystem.B": Pre-10.7 macOS stores symbols in monolithic libraries instead of split sub-libraries
- "7/10 tools" and "8/10 tools": BaseSystem images don't include osascript, dscl, or tmutil (these are present in the full installed OS)
- "dyld cache": Big Sur+ moved system libraries into a shared cache; symbols are present but not inspectable via nm
- "runtime": Tahoe verified by running the agent directly, not from installer inspection
- Monterey through Sequoia use a new SharedSupport.dmg payload format that cannot be deep-verified without installing into a VM

## Deep Verification Details

Installer images are analyzed by `scripts/verify-installer.sh` which checks:

- **Apple16X50Serial.kext** presence, bundle version, and IOPCIClassMatch (must be `0x07000000&0xFFFF0000` = PCI class 0x0700 Serial Controller, which matches QEMU ISA serial)
- **All required libc symbols** as defined by the agent's checked-in undefined-symbol baseline (`tests/legacy_slice_symbols_x86_64.txt` and `…_i386.txt`, ~133 libc symbols after framework and weak imports are separated out). Versioning suffixes (`$INODE64`, `$1050`, etc.) are stripped during the check because nm reports the unversioned name. The installer verifier derives the required-symbol list directly from this baseline rather than hand-curating it — the baseline IS the source of truth for what the agent links against, and an installer that lacks any of those symbols will fail to host the agent.
- **1 weak-imported symbol**: `host_statistics64` — present on 10.6+, absent on 10.4 Tiger. Agent's i386 and x86_64 slices weak-import this via `__attribute__((weak_import))` in `src/cmd-hardware.c` so dyld resolves it to NULL on Tiger instead of refusing to load; the agent falls back to `vm_stat` text parsing when the symbol is absent. The verifier reports present/absent without failing.
- **14 framework symbols** in the baseline (`_CF*`, `_kCF*`, `_IO*`) — checked at the framework-directory level (CoreFoundation.framework, IOKit.framework presence) rather than per-symbol, since the framework binaries are stable Apple-managed surfaces.
- **CoreFoundation.framework** and **IOKit.framework** presence
- Required tools: `sw_vers`, `diskutil`, `sysctl`, `shutdown`, `launchctl`
- APFS support: `diskutil` APFS references, `tmutil localsnapshot` availability

### Kext Version Timeline

| macOS | Apple16X50Serial.kext | IOSerialFamily.kext | PCI Class Match |
|---|---|---|---|
| 10.4.5 Tiger (Intel DVD) | v1.7 | v9.0.0d30 | 0x07000000&0xFFFF0000 |
| 10.4.11 Tiger (combo update) | v1.9 | v8.0.0d28 | 0x07000000&0xFFFF0000 |
| 10.5.4 Leopard | v1.9 | v9.1 | 0x07000000&0xFFFF0000 |
| 10.6.3 Snow Leopard | v3.0 | v10.0.3 | 0x07000000&0xFFFF0000 |
| 10.7.5 Lion | v3.0 | v10.0.5 | 0x07000000&0xFFFF0000 |
| 10.8.5 Mountain Lion | v3.1 | v10.0.6 | 0x07000000&0xFFFF0000 |
| 10.9.5 Mavericks | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.10.5 Yosemite | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.11.6 El Capitan | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.12.6 Sierra | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.13.6 High Sierra | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.14.6 Mojave | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 10.15.7 Catalina | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 11.6 Big Sur | v3.2 | v11 | 0x07000000&0xFFFF0000 |
| 26.3 Tahoe | v3.2 | v11 | 0x07000000&0xFFFF0000 |

The ISA serial driver has been present since Mac OS X 10.4 Tiger (2005). The PCI class match (`0x07000000&0xFFFF0000`) has been identical across every version — the driver recognizes QEMU's ISA serial port the same way on Tiger as it does on Tahoe. The kext version has evolved (v1.6 PPC → v1.7 Intel → v1.9 combo update → v3.0 Snow Leopard → v3.2 Mavericks through Tahoe) but the PCI personality is functionally unchanged.

### Binary Evidence

As of v2.5.0 the release ships a single tri-fat universal binary; the table below describes each slice within it.

| Slice | Deployment Target | Entry Point | Load Command | Frameworks | Undef Symbols |
|---|---|---|---|---|---|
| i386 | 10.4 (Tiger) | LC_UNIXTHREAD | LC_VERSION_MIN_MACOSX | CoreFoundation, IOKit, libSystem.B | 147 |
| x86_64 | 10.6 (Snow Leopard) | LC_UNIXTHREAD | LC_VERSION_MIN_MACOSX | CoreFoundation, IOKit, libSystem.B | 147 |
| arm64 | 11.0 (Big Sur) | LC_MAIN | LC_BUILD_VERSION | CoreFoundation, IOKit, libSystem.B | 148 |

All three slices link only against system frameworks (CoreFoundation, IOKit) and libSystem.B.dylib. No third-party dependencies. Legacy slices (i386, x86_64) use `LC_UNIXTHREAD` via `-Wl,-ld_classic` + `-mmacosx-version-min=N.N` + legacy 10.13 SDK so 10.4-10.7 dyld can load them (10.8+ introduced `LC_MAIN` which older dyld rejects — see issue #4 / CHANGELOG v2.5.0). Per-slice symbol baselines live at `tests/legacy_slice_symbols_<arch>.txt` (i386 + x86_64 + arm64, all required) and are diffed by `scripts/verify-legacy-slices.sh` on every CI build. The arm64 baseline (added in audit wave 5 / MED-1) guards the Big Sur API floor — a future direct import of a macOS 12+ symbol would slip the `minos 11.0` declaration without triggering a load-command-level error, so the symbol-set diff is the actual gate.

## Architectural Transitions Covered

| Transition | macOS Version | Impact | Our Handling |
|---|---|---|---|
| HFS+ → APFS | 10.13 High Sierra | Freeze mechanism changes | Runtime detection, APFS snapshot on 10.13+, sync-only fallback |
| No SIP → SIP | 10.11 El Capitan | Kext loading restricted | Not applicable (ISA serial uses built-in kext) |
| Intel → Apple Silicon | 11.0 Big Sur | Architecture change | arm64 slice inside the tri-fat universal binary; dyld selects at load time |
| No VirtIO → Native VirtIO | 11.0 Big Sur | Apple's `AppleQEMUGuestAgent` launches on VZ-backed hosts only — see [ISA Serial Transport — Why](#isa-serial-transport--why) | ISA serial only as of v2.5.0; v2.4.x fallback VirtIO entries removed from `src/channel.c known_devices[]`. Operators on configurations that previously relied on the fallback (UTM Emulate default, custom QEMU without `-device isa-serial`) must reconfigure the hypervisor to present an ISA UART — see CHANGELOG v2.5.0 BREAKING |
| PPC → Intel | 10.4.4 Tiger | Architecture change | i386 slice inside the universal binary for 10.4–10.5; x86_64 slice from 10.6 |
| 32-bit → 64-bit only | 10.6 Snow Leopard | Binary architecture | x86_64 slice from 10.6; i386 slice for older — both inside the universal binary |
| Monolithic libc → split sub-libs | 10.7 Lion | Library layout | Symbol check adapts: libc.dylib → libSystem.B → sub-libraries |
| Split sub-libs → dyld shared cache | 11.0 Big Sur | Library layout | Symbols not inspectable via nm but resolve at runtime |
| bash → zsh default | 10.15 Catalina | Shell for guest-exec | Uses /bin/sh (always available), not login shell |

## Recommended Anchor VMs for Expanded Validation

| macOS | Priority | Why |
|---|---|---|
| 10.4 Tiger | High | Oldest supported, kext v1.9, i386 slice of the universal binary, validates floor |
| 10.13 High Sierra | High | APFS transition — validates freeze snapshot path via tmutil |
| 11.0 Big Sur | High | VirtIO + modern stack — validates both transports |
| 15.x Sequoia | High | Current stable release — validates nothing has regressed |

## PowerPC and Pre-10.4 Versions (Not Currently Supported)

QEMU can emulate PowerPC Macs via its `mac99` (G4) and `g3beige` (G3) machine types. The following Apple operating systems can boot on QEMU PPC but are **not currently supported** by the guest agent:

| OS | QEMU PPC | Why Not Supported |
|---|---|---|
| Mac OS 9.0–9.2 | Boots and runs | Classic Mac OS, no POSIX/Unix layer, completely different OS |
| Mac OS X 10.0 Cheetah | Boots and runs | No Apple16X50Serial.kext (kext first appears in 10.4) |
| Mac OS X 10.1 Puma | Boots and runs | No Apple16X50Serial.kext |
| Mac OS X 10.2 Jaguar | Boots and runs | No Apple16X50Serial.kext |
| Mac OS X 10.3 Panther | Boots and runs | No Apple16X50Serial.kext (unverified — may exist) |
| Mac OS X 10.4 Tiger (PPC) | Boots and runs | Kext exists (v1.6) but PPC binary required |
| Mac OS X 10.5 Leopard (PPC) | Boots and runs | Kext exists (v1.9) but PPC binary required |

### What Would Be Needed for PPC Support

1. **PPC cross-compiler.** Apple removed PPC support from Xcode after version 3.x. Building a PPC Mach-O binary requires either an old Xcode installation or a cross-compilation toolchain like `powerpc-apple-darwin-gcc`.

2. **Serial transport investigation.** QEMU's PPC mac99 machine emulates a Zilog 85C30 ESCC (Enhanced Serial Communications Controller), not the 16550 UART that `Apple16X50Serial.kext` matches. The agent would need to connect via ESCC serial paths (likely `/dev/cu.modem` or `/dev/tty.serial`) or USB serial (`-device usb-serial`). This needs testing on an actual PPC VM.

3. **API compatibility audit.** Mac OS X 10.0–10.3 may be missing POSIX/Mach APIs the agent depends on (`getifaddrs`, `getutxent`, `host_statistics64`, etc.). The 20 critical symbol checks from our installer verification would need to pass.

4. **Testing infrastructure.** PPC VMs for each target version, ability to SCP binaries in and run tests.

### How to Contribute PPC Support

If you are actively running PPC Mac OS X VMs and want to help:

1. Open an issue at [github.com/mav2287/mac-guest-agent/issues](https://github.com/mav2287/mac-guest-agent/issues)
2. Tell us what OS version, QEMU machine type, and host platform you're using
3. Check what serial devices exist in your PPC VM (`ls /dev/cu.*  /dev/tty.*`)
4. Check if Apple16X50Serial.kext is present (`ls /System/Library/Extensions/Apple16X50Serial.kext`)

We're open to PPC support but need contributors with real PPC VM environments to help test.

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

Boot a VM from the installer on PVE, install the agent, then run a single host-side command:

**Inside the VM — install the agent (one-time):**

```bash
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

**From the PVE host — run the unified verifier (single command):**

```bash
curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh | bash -s -- <vmid> | tee verify.txt
```

`scripts/verify.sh` is the single multi-transport verifier — auto-detects PVE (this section), libvirt, or UTM from the host environment, or accepts `--transport <name>` explicitly. On PVE it runs a single end-to-end pass:

- **Preflight** — root check, cluster locality (multi-node clusters: verifier refuses to run if VM lives on a different node), backup lock (refuses to run if `vzdump` is in progress).
- **Host-side checks** — config (`agent: enabled=1,type=isa`, `discard=on`, `ssd=1`), VM running, `ping`, `get-osinfo`, `network-get-interfaces`, `info` (the 42-command list + version), agent-sourced memory (block-info × blocks).
- **Host Environment capture (schema 2.0)** — `sw_vers`, `sysctl` hardware (model / CPU / memory / brand string), `kextstat` (filtered to `Apple16X50Serial` / `AppleVirtIO` / `IOSerialFamily`), `ioreg` serial / virtio nodes, parsed mount table, `launchctl list com.macos.guest-agent`, agent-log `stat`. All assembled into a single `host_environment` object in the JSON appendix.
- **Multi-cycle freeze/thaw with behavioural verification by content** — runs `--freeze-cycles N` (default 3) consecutive freeze/thaw cycles to catch state-leak bugs between cycles. Each cycle verifies the agent genuinely gates non-freeze commands while frozen by inspecting *response content* (not `qm` exit code; see [docs/research/UPSTREAM_NOTES.md Target 4](research/UPSTREAM_NOTES.md) for the reasoning) and recovers after thaw. An auto-thaw safety trap fires on Ctrl-C / crash to prevent leaving the VM frozen.
- **Mount-dispatch cross-check** — compares the captured mount table to the actual frozen count from the last cycle (excluding network / special / read-only mounts the dispatch table categorically skips). Catches dispatch drift between `fs_dispatch_class()` and the live runtime.
- **In-VM diagnostics via `qm guest exec`** — drives `mac-guest-agent --self-test-json` and `mac-guest-agent --safe-test-json` from the host. Validates the `freeze_dispatch` contract (per-FS table + cpustats discriminator) and the read-only command sweep. Tails the agent log for the per-event "Filesystem frozen:" INFO line so the per-treatment breakdown surfaces in the report.
- **PII redaction** is on by default — IPv4, IPv6 (full / compressed / link-local), MAC, hostnames (PVE host + cluster nodes + guest), and the supplied VM ID are replaced before output. Disable with `--no-redact` if you want raw values.
- **Structured JSON appendix** at the end of the report — paste it into `docs/evidence/<version>/verify.json`. Run `verify.sh --help` for all flags (`--no-redact`, `--no-appendix`, `--no-in-vm`, `--no-env-capture`, `--no-freeze`, `--freeze-cycles N`).

A version is promoted to **Tier 1** when `verify.sh` reports `ALL CHECKS PASSED` (and, by implication, both in-VM JSON diagnostics succeed inside the same run).

### Storing Results

Internally, JSON outputs are kept under `results/` (gitignored) during development and referenced by build number in the Evidence column of the matrix above.

External contributors: paste the `verify.sh` text output and the JSON appendix in a GitHub issue comment, or open a small PR adding them under `docs/evidence/<version>/` (as `verify.txt` and `verify.json`) and updating the corresponding matrix row.

## Quality Metrics

| Metric | Value |
|---|---|
| Static analysis (clang --analyze) | 0 bugs across 21 source files |
| Memory leaks (macOS leaks tool) | 0 leaks, 1143 allocations, 173KB |
| Fuzz testing (ASAN, 210k rounds) | 0 crashes |
| Linux qemu-ga command parity | 42 registered. Intentionally not registered on macOS (no equivalent API, matches upstream gating): `guest-fstrim` (no on-demand TRIM; see RECLAIM.md), `guest-get-devices` (Windows-only). `guest-network-get-route` is implemented. |
| Code coverage | 55.88% line, 80.95% function (remainder requires root, real hardware, or destructive operations) |
| Test suite | unit + proactive + fuzz (210k rounds) + integration + verify-transports + install-flags (current counts in `make test` output) |
