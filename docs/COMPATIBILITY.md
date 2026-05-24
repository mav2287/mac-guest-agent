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

## ISA Serial Transport — Why

macOS can be a guest in two distinct virtualisation environments, and they treat the QGA transport differently:

| Host class | Examples | Apple's `AppleQEMUGuestAgent` behaviour | VirtIO channel availability |
|---|---|---|---|
| **Apple Virtualization.framework** | UTM (VZ backend), `vz_run`, anything backed by `VZVirtualMachine` | IOKit-launched on-demand once the `AppleVirtIOAgentDevice` property is matched by the `applevirtio.console` driver | Claimed by Apple's agent |
| **Plain QEMU/KVM** | Proxmox VE, libvirt, raw QEMU (typically with OpenCore for macOS guests) | Never launches — the `applevirtio.console` driver does not load on QEMU, so `AppleVirtIOAgentDevice` is never set and the IOKit match never fires | Technically free |

The historical "use ISA because Apple claims VirtIO" framing is correct for the first row and oversimplified for the second. On Proxmox/QEMU/OpenCore guests, Apple's agent isn't running and the VirtIO console channel would in fact be available. We still default to ISA universally for two reasons:

1. **One transport across both host classes.** Detecting which environment we're in at runtime would require IOKit introspection and would diverge the launchd plist between VZ and QEMU installs. Using ISA everywhere keeps the install, the launchd config, and the channel-detection list (`src/channel.c`) identical on every macOS guest.
2. **No conflict with Apple's agent on VZ.** If a user moves a disk image from QEMU to UTM (or vice versa), an ISA-based install keeps working without reinstall. A VirtIO-based install would silently conflict with `AppleQEMUGuestAgent` the moment the same image boots under Virtualization.framework.

The cost is using a slightly older transport. `Apple16X50Serial.kext` has been present on every macOS from 10.4 Tiger (2005) through 26.3 Tahoe (2026) with an identical PCI class match — see [Kext Version Timeline](#kext-version-timeline) below — so this cost is essentially zero in practice. Apple's own `AppleQEMUGuestAgent`, where it does launch, also speaks the QGA protocol but ships only 18 commands and no filesystem freeze (per local inspection on macOS 26.5; see `docs/research/UPSTREAM_NOTES.md` Target 6 for the symbol survey). That feature gap, not the transport question, is the primary reason this project exists.

## macOS Version Matrix

| macOS | Tier | Binary | Launches | Self-test | PVE Integration | ISA Serial | Freeze | Evidence |
|---|---|---|---|---|---|---|---|---|
| **10.4 Tiger** | **1** | **i386** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v1.9** | Untested | **Intel DVD: kext v1.7 (i386+ppc). Combo update: v1.9. Same PCI class match. i386 binary required. `host_statistics64` absent — weak-imported, `vm_stat` text fallback (v2.4.1+). Serial transport `poll()`→`select()` in v2.4.2 — macOS `poll()` is kqueue-backed and the serial BSD client on 10.4 doesn't support the kqueue readiness path, so `poll()` returned `POLLNVAL` for `/dev/cu.serial1`. v2.4.1 `--safe-test` 21/21 PASS on 10.4.11; v2.4.2 confirmed serving over PVE — ping, get-osinfo, network, memory display, reboot/shutdown — on 10.4.11 by @vit9696 (issue #2). Freeze not yet exercised.** |
| **10.5 Leopard** | **2** | **i386 only** | Untested | Untested | Untested | **Kext v1.9** | Untested | **Deep verify 4/4: kext + symbols (in libc.dylib) + frameworks + PCI 0x0700. i386 binary required.** |
| **10.6 Snow Leopard** | **2** | **x86_64 + i386** | Untested | Untested | Untested | **Kext v3.0** | Untested | **Deep verify 4/4: kext + symbols (in libSystem.B) + frameworks + PCI 0x0700. Deployment target.** |
| **10.7 Lion** | **2** | **x86_64 + i386** | Untested | Untested | Untested | **Kext v3.0** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.8 Mountain Lion** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.1** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.9 Mavericks** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.10 Yosemite** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.11 El Capitan** | **1** | **x86_64** | **Yes** | **Yes** | **Yes (PVE)** | **Kext v3.2** | **Yes (HFS+)** | **v2.4.3 verifier 38/0 on real Xserve3,1 metal — see [`evidence/10.11.6/`](evidence/10.11.6/). 3 freeze cycles all clean (sync + F_FULLFSYNC on HFS+ root, 4 special FS skipped). 290/290 stress, mount-verified snapshot. Deep verify 4/4** |
| **10.12 Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + frameworks + PCI 0x0700** |
| **10.13 High Sierra** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.14 Mojave** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **10.15 Catalina** | **2** | **x86_64** | Untested | Untested | Untested | **Kext v3.2** | Untested | **Deep verify 4/4: kext + 20/20 symbols + APFS + diskutil APFS** |
| **11.6 Big Sur** | **2** | **arm64 + x86_64** | Untested | Untested | Untested | **Kext v3.2 + VirtIO** | Untested | **Deep verify 4/4: kext + symbols (dyld cache) + frameworks + VirtIO v74.120.4** |
| 12.6 Monterey | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| 13.0 Ventura | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| 14.0 Sonoma | 3 | arm64 + x86_64 | Untested | Untested | Untested | Untested | Untested | Installer present. SharedSupport payload format — cannot deep-verify without VM. |
| **15.7 Sequoia** | **1** | **arm64 + x86_64** | **Yes** | **Yes** | **Yes (PVE)** | **ISA serial** | Untested | **External user confirmed on 15.7.5: self-test pass, PVE integration, VirtIO disk + ISA serial (pgcudahy, PR #1)** |
| **26.3 Tahoe** | **1** | **arm64 + x86_64** | **Yes** | **Yes** | **No (test mode only)** | **Kext v3.2** | **Yes (APFS, dry-run)** | **48 unit + 31 proactive + 62 integration tests** |

## Sub-Evidence Matrix

Detailed breakdown of what has been verified per version. All installer-verified versions (Tier 2) pass all four core checks.

| macOS | Serial Driver | C Library Symbols | Frameworks | Required Tools | APFS/VirtIO | Binary Target |
|---|---|---|---|---|---|---|
| 10.4.11 Tiger | v1.9, PCI 0x0700 | 19/19 required (no host_statistics64) | CF + IOKit | in pkg | — | i386 |
| 10.5 Leopard | v1.9, PCI 0x0700 | in libc.dylib | CF + IOKit | 7/10 | — | i386 |
| 10.6 Snow Leopard | v3.0, PCI 0x0700 | in libSystem.B | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.7 Lion | v3.0, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.8 Mountain Lion | v3.1, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.9 Mavericks | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.10 Yosemite | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 7/10 | — | x86_64 10.6 |
| 10.11 El Capitan | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.6 |
| 10.12 Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | — | x86_64 10.6 |
| 10.13 High Sierra | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
| 10.14 Mojave | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
| 10.15 Catalina | v3.2, PCI 0x0700 | 20/20 | CF + IOKit | 8/10 | APFS, diskutil APFS | x86_64 10.6 |
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
- **19 required C library symbols** in system sub-libraries: `getifaddrs`, `freeifaddrs`, `getutxent`, `endutxent`, `getloadavg`, `getmntinfo`, `getpwnam`, `sysctlbyname`, `gettimeofday`, `settimeofday`, `host_statistics`, `poll`, `strtok_r`, `fcntl`, `sync`, `tcgetattr`, `tcsetattr`, `tcflush`, `tcdrain`
- **1 optional symbol** (weak-imported): `host_statistics64` — present on 10.6+, absent on 10.4 Tiger. Agent falls back to `vm_stat` text parsing when unavailable.
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
| No VirtIO → Native VirtIO | 11.0 Big Sur | Apple's `AppleQEMUGuestAgent` launches on VZ-backed hosts only — see [ISA Serial Transport — Why](#isa-serial-transport--why) | ISA serial primary on all host classes for symmetry; VirtIO devices in `src/channel.c` covered as a fallback for hosts where ISA isn't presented (UTM with the QEMU backend, custom QEMU configs without `-device isa-serial`) |
| PPC → Intel | 10.4.4 Tiger | Architecture change | i386 Makefile target for 10.4–10.5, x86_64 from 10.6 |
| 32-bit → 64-bit only | 10.6 Snow Leopard | Binary architecture | x86_64 target from 10.6, i386 Makefile target for older |
| Monolithic libc → split sub-libs | 10.7 Lion | Library layout | Symbol check adapts: libc.dylib → libSystem.B → sub-libraries |
| Split sub-libs → dyld shared cache | 11.0 Big Sur | Library layout | Symbols not inspectable via nm but resolve at runtime |
| bash → zsh default | 10.15 Catalina | Shell for guest-exec | Uses /bin/sh (always available), not login shell |

## Recommended Anchor VMs for Expanded Validation

| macOS | Priority | Why |
|---|---|---|
| 10.4 Tiger | High | Oldest supported, kext v1.9, i386 binary, validates floor |
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
- **Host-side checks** — config (`agent: enabled=1,type=isa`, `discard=on`, `ssd=1`), VM running, `ping`, `get-osinfo`, `network-get-interfaces`, `info` (the 45-command list + version), agent-sourced memory (block-info × blocks).
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
| Linux qemu-ga command parity | 45 registered. 1 Linux-side command not implemented: guest-get-devices (Windows-only). guest-network-get-route now implemented. |
| Code coverage | 55.88% line, 80.95% function (remainder requires root, real hardware, or destructive operations) |
| Test suite | 48 unit + 31 proactive + 210k fuzz + 63 integration |
