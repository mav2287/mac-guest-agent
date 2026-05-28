# universal_upgrade.md — v2.4.4 Single-Universal-Binary Migration

> **STATUS: HISTORICAL / SUPERSEDED (2026-05-26).** This plan has shipped.
> Authoritative sources for the as-built v2.4.4 state are the repository
> itself: `Makefile`, `.github/workflows/build.yml`, `.github/workflows/release.yml`,
> `scripts/verify-legacy-slices.sh`, `scripts/install.sh`, `CHANGELOG.md`,
> and `docs/COMPATIBILITY.md`. Do not edit this plan to track new work —
> it is kept only as a record of the design rationale that produced v2.4.4.
>
> Known divergences from the as-built state (do not "fix" here — they exist
> because reality moved during execution):
>
> - §4.5 / D16 / H04 describe the Linux `surrogate-32bit` job as building
>   `protocol.c` + `util.c` + `cJSON.c`. The actual workflow builds only
>   `protocol.c` + `cJSON.c`; `util.c` was excluded because it includes
>   `compat.h` and uses POSIX surface that needs `_POSIX_C_SOURCE`. See
>   the comment block above `surrogate-32bit:` in `.github/workflows/build.yml`.
> - §6 implies "no CI runner image pinning"; both workflows are pinned to
>   `macos-14` with a `macos-latest` canary, documented inline in the
>   workflows.
> - §3 / §5 cover the legacy-slice fix in terms of the legacy SDK +
>   `-mmacosx-version-min` flag alone. The final shipped recipe also
>   requires `-Wl,-ld_classic` to invoke Apple's older linker; ld-prime
>   (Xcode 15+) hardcodes LC_MAIN for x86_64 regardless of the min-flag.
>   This is the load-bearing workaround and is documented in `Makefile`
>   above `build-i386` / `build-x86_64`.

**Plan version:** 2.4 (fourth spot-check audit findings H01-H08 addressed, 2026-05-25)
**Status:** Execution-ready pending James sign-off (§14). Third-party reviewer block waived per project owner. Not committed. Not gitignored.
**Author:** drafted via Claude session
**Owner of execution:** James (mav2287)
**Subject binary:** `mac-guest-agent` (C, LaunchDaemon, root-owned, `/usr/local/bin`)
**Target release:** v2.4.4
**Audits addressed:** `audit.md` (findings A1, A2) + 20 inline plan audit findings from v1

---

## 0. How to read this document

Every claim in §3 (Verified Findings) is either confirmed by a reproducible command transcript or cited to Apple open-source code at a specific repository path. Every step in §5 (Implementation) has at least one verification gate (`✓`). The risk register in §9 enumerates every failure mode I could surface, with mitigation and detection for each. §12 lists open questions for the reviewer.

For a third-party reviewer reading cold: §1–§4 should be enough context to evaluate intent. §5–§6 are the actual change set, audit-readable. §11 lists what is *not* being shipped, so scope creep is detectable. §13 sign-off blocks are the gate before any code change.

This is plan **v2.4**. Audit progression so far:
- v1 → v2: P-series (20 findings, all valid, all addressed)
- v2 → v2.2: F-series (16 findings, all valid, all addressed)
- v2.2 → v2.3: G-series (12 findings, all valid, all addressed)
- v2.3 → v2.4: H-series (8 findings, all valid, all addressed — this round)

Trend is downward (20 → 16 → 12 → 8). Each finding is verified against the actual repo state via fresh tests (build experiments, lipo argument-order reproduction, util.c POSIX surface inspection, etc.) before applying the fix; none accepted at face value.

Concretely, F01's umbrella concerns were:
- `selected_arch` code used cJSON when `src/selftest.c::emit_system_info()` is pure printf → **F07 fix in §6.9**
- `-m32` surrogate included non-portable files (selftest.c drags compat/cmd-fs deps; log.c lacks `<stdint.h>`) → **F04 + F05 fix in §4.5 and §5 Step 4**
- Verifier didn't catch unknown `LC_REQ_DYLD` because awk-grep only matched `cmd LC_*` not numeric `cmd ?(0x80…)` → **F10 fix in §6.2**
- §8 R-0 contradicted D10's no-self-help decision → **F16 fix in §8**

The remaining 12 findings (F02 install.sh coupling, F03 stale 4.1 bullet, F06 release-workflow tests the wrong binary, F08 wrong SDK URL in error message, F09 dist target leaks stale files, F11 blocklist not allowlist, F12 chmod +x missing, F13 troubleshooting assumes binary loads, F14 modern-machine caveat dropped, F15 CHANGELOG SDK mismatch) all addressed in their respective sections.

**v2.3 round (G01-G12) addressed:**
- §4.1 bullet 5 + §6.4 build.yml diff + §6.10 CHANGELOG: surrogate scope now consistently excludes `log.c`/`selftest.c` (G02, G10, G12)
- §5 Step 1: sequencing corrected — full uncommitted application of all v2.4.4 changes, not just Makefile (G03)
- §5 Step 3: sabotage now deterministic — uses i386 min=10.6 (empirically triggers `LC_DYLD_INFO_ONLY`; 10.5 does not), per fresh Step 0-style test on this repo (G04)
- §6.1 Makefile diff: corrected the `@if [ ! -d $(I386_SDK) ]` error message body (was still saying `MacOSX10.6.sdk.tar.xz`), not just the comment (G06)
- §6.2 verifier: `lipo` arg order fixed (`lipo "$BIN" -verify_arch …`, verified locally returns exit 1 wrong-order vs exit 0 correct-order); missing baseline files = hard FAIL for legacy slices, not INFO (G07, G08); comprehensive known-LC list with per-slice REQ_DYLD subset — any unknown LC name now also fails (G09)
- §6.5a install.sh: PPC-rejection guard preserved as `validate_arch()` even though the asset is universal (G11)
- §11: `<stdint.h>` fix for `util.c` brought into v2.4.4 scope as **tech-debt hygiene** (D20). Originally added in v2.3 because the surrogate compiled util.c (G05); v2.4 H04 dropped util.c from the surrogate scope entirely (compat.h dependency + POSIX feature-test macros). The util.c stdint include is kept anyway — one-line risk-free cleanup that paves the way for future surrogate expansion. `log.c` stays out.

---

## 1. Executive summary

**Problem.** `mac-guest-agent-darwin-amd64` v2.4.3 crashes at startup on macOS 10.6 Snow Leopard and 10.7 Lion with `dyld: unknown required load command 0x80000028` → SIGTRAP. The binary's header claims `LC_VERSION_MIN_MACOSX 10.6` but its entry-point load command is `LC_MAIN`, introduced in 10.8 Mountain Lion. Reported as GitHub issue #4 by @vit9696 on 2026-05-25.

**Root cause** (precise version). The v2.4.3 artifact was built by GitHub Actions' `macos-latest` runner at release time (2026-05-24), which carried **Xcode 15.5**. Xcode 15.5's ld64 emits `LC_MAIN` for x86_64 even when the build sets `MACOSX_DEPLOYMENT_TARGET=10.6` via env var, because the env var is silently clamped against the SDK's effective deployment-target floor. The current dev machine's Xcode (26.5) does *not* exhibit this clamp under identical Makefile inputs — it produces `LC_UNIXTHREAD` correctly. So the bug is real and present in the released artifact, but it is **toolchain-version-dependent**, not a universal property of all modern Xcode. This matters because (a) it means we cannot rely on local-toolchain-produces-LC_UNIXTHREAD as evidence the next release is safe, and (b) the CI gate codifying the invariant is therefore the load-bearing piece of the fix, not the Makefile change alone.

**Fix shape (v2.4.4).**

1. **Linker-level fix.** Rebuild the x86_64 slice with `-isysroot $(LEGACY_SDK) -mmacosx-version-min=10.6` (and equivalently for i386 with explicit `-mmacosx-version-min=10.4`). The legacy SDK + explicit min flag combination is verified to emit `LC_UNIXTHREAD` deterministically (§3.1).
2. **CI invariant.** Add `scripts/verify-legacy-slices.sh`, invoked by both `.github/workflows/build.yml` and `.github/workflows/release.yml`, that fails the build if any disallowed load command, missing required load command, off-spec deployment target, unexpected dylib dependency, or unsafe undefined symbol appears in the i386 or x86_64 slice. **This is the actual invariant; the Makefile change is belt-and-suspenders.**
3. **Universal-only release.** Collapse the release artifacts to one tri-fat `mac-guest-agent-darwin-universal` containing `i386 + x86_64 + arm64`. The previous thin per-arch artifacts (`-i386`, `-amd64`, `-arm64`) are no longer published. dyld picks the appropriate slice at load time; one binary covers macOS 10.4 → 26.
4. **Non-VM CI surrogate.** Add a hosted Linux `-m32` job that builds the genuinely-portable subset (`protocol.c` + `cJSON.c` only — per H04, `util.c` includes `compat.h` and uses POSIX surface that requires feature-test macros; `selftest.c` drags compat/macOS deps; `log.c` lacks `<stdint.h>`) via a standalone `tests/surrogate_32bit_main.c` driver. Catches pointer-width / struct-layout / integer-conversion regressions in protocol-marshaling and cJSON without requiring access to old Intel Mac hardware or community testers.
5. **Source code cleanup.** Update `src/service.c:149-150` (the `--update` flag's hint message), `Makefile` targets `dist`/`pkg`/`sign`/`dsym`/`help`, `scripts/build-pkg.sh`, `scripts/verify-installer.sh`, and all user-facing docs to reflect the universal-only distribution shape.
6. **Diagnostic addition.** Extend `--self-test-json`'s `system_info` to include a `selected_arch` field, so verify.sh evidence drops capture which slice dyld picked on the target host — useful for post-incident forensics now that there is no thin-artifact escape hatch.

**H01 fix:** Executive summary bullet 4 now matches §4.1/§4.5/§6.4 — standalone driver, narrower scope (also narrowed further per H04 below).

**Blast radius.** Small. Project has one known reproducible-issue reporter (vit9696, who triaged the bug). All other known production users are on 10.4 (i386 slice, unaffected) or 10.11+ (LC_MAIN works, unaffected). CI runs on macOS 14/15/26 — none affected. The migration also corrects a pre-existing documentation lie that has shipped since v2.4.0.

**The one real constraint, now more dangerous.** dyld does *not* fall back to another slice if the chosen slice fails to load. With universal-only distribution (no thin artifacts), if a user hits an unexplained slice-load failure on some unanticipated host, the escape hatch is gone. Mitigations in §4.5.

---

## 2. Problem statement & background

### 2.1 The reported bug

GitHub issue #4 (2026-05-25, vit9696), title *"macOS 10.5/10.6 compatibility with amd64"*. Reproducer on a 10.6.8 guest:

```
sh-3.2# gdb $(which mac-guest-agent)
...
unable to read unknown load command 0x80000028
...
sh-3.2# otool -l /usr/local/bin/mac-guest-agent
...
Load command 10
   cmd ?(0x80000028) Unknown load command
```

Vit9696's diagnosis: *"I believe it is happening due to LC_MAIN vs LC_UNIX_THREAD command. Older macOS most likely cannot find GA entry point for x86_64."* This diagnosis is correct (see §3.1 and §3.3).

Same crash occurs on 10.5.8. Vit9696 has been using `mac-guest-agent-darwin-i386` on both as a workaround.

### 2.2 What `mac-guest-agent` is, for a reviewer reading cold

A QEMU Guest Agent (QGA) implementation for macOS guests running on hypervisors that speak QGA (Proxmox VE, libvirt/KVM, UTM, raw QEMU). A small (~130 KB per arch) C daemon that runs as root under a LaunchDaemon, listens on an ISA serial port for QGA-protocol JSON commands from the host, and responds with structured data (network info, memory stats, OS info, filesystem freeze/thaw, etc.). Supports macOS 10.4 Tiger through 26 Tahoe. Distribution: `curl` from GitHub Releases. No package manager, no signing, no notarization. Source at `github.com/mav2287/mac-guest-agent`.

### 2.3 Current binary distribution (v2.4.3, as of 2026-05-24)

Four release artifacts:

| File | Arch | Declared min | Actual min | Built on |
|---|---|---|---|---|
| `mac-guest-agent-darwin-i386` | i386 | `LC_VERSION_MIN_MACOSX 10.4` | 10.4 | Phracker `MacOSX10.13.sdk`; `LC_UNIXTHREAD` |
| `mac-guest-agent-darwin-amd64` | x86_64 | `LC_VERSION_MIN_MACOSX 10.6` (`sdk 15.5`) | **10.8** (lies) | Xcode 15.5 SDK; `LC_MAIN` |
| `mac-guest-agent-darwin-arm64` | arm64 | `LC_BUILD_VERSION sdk=15.5 minOS=11.0` | 11.0 | Xcode 15.5 SDK; `LC_MAIN` |
| `mac-guest-agent-darwin-universal` | x86_64+arm64 | — | 10.8 / 11.0 | `lipo` of amd64 + arm64. **No i386 slice.** Inherits x86_64 LC_MAIN bug. |

### 2.4 Why the bug exists — precise account

Two facts conspire in the GitHub Actions environment used at v2.4.3 release time:

1. **Xcode 15.5's ld64 emits `LC_MAIN` for x86_64 when the effective deployment target resolves to ≥ 10.8.** The release Makefile sets `MACOSX_DEPLOYMENT_TARGET=10.6` via env var. Per `apple-oss-distributions/ld64` (`src/ld/Options.cpp`, the entry-point gate `if (platforms().minOS(version2012) || arch == CPU_TYPE_ARM64) { fEntryPointLoadCommand = true; }`, with `version2012 = mac10_8` in `src/ld/ld.hpp`), the gate should resolve to false for min=10.6 and emit `LC_UNIXTHREAD`. In Xcode 15.5's actual behaviour, it does not: the env-var-supplied target is silently clamped against the SDK's effective floor in some code paths, producing min=10.6 in the load command but LC_MAIN as the entry point.

2. **The Makefile relies on the env var alone, with no explicit `-mmacosx-version-min` flag.** When the env var is honoured (newer Xcode), the build is correct. When it is silently clamped (Xcode 15.5), the build is wrong. There is no in-tree guard.

**Important toolchain caveat.** On the current dev machine (Darwin 25.5.0 / Xcode 26.5 / clang 21.0.0 / SDK 26.5), the unmodified `make build-x86_64` produces `LC_VERSION_MIN_MACOSX 10.6` and `LC_UNIXTHREAD`, *not* `LC_MAIN`. Reproduced 2026-05-25 — transcript in §3.1. This means:

- The bug is real and present in the released v2.4.3 artifact (confirmed in the artifact itself; see §3.0 below).
- "Modern Xcode silently clamps the env var" is *not* a universal property; it depended on Xcode 15.5's specific behaviour at release time.
- The next GitHub Actions runner image change could either repair the bug accidentally (newer Xcode honours the env var) or reintroduce it (Apple changes ld64 default again, or `macos-latest` rolls back).
- We cannot rely on local toolchain behaviour as evidence about released artifacts. We need a build-time invariant.

This is the fundamental argument for §5 Step 3 (`scripts/verify-legacy-slices.sh`) being a permanent fixture rather than a one-time fix.

The `-Wl,-no_new_main` linker flag is not a workable mitigation: the body of that option in current ld64 is literally `// HACK until 39514191 is fixed` (`Options.cpp` ~line 4237 in current `main` branch). Accepted to keep build systems from erroring, no behaviour change.

---

## 3. Verified findings

All experiments below are reproducible. Each subsection includes the exact command, the exact output, and (for source citations) the repository path with the function name to grep for. Apple open-source URLs reference the `main` branch of `apple-oss-distributions` as of plan v2 draft date (2026-05-25); for stable citation use the most recent commit hash from those branches at sign-off time.

### 3.0 Environment fingerprint (so future reviewers can re-run cleanly)

```
$ sw_vers
ProductName:        macOS
ProductVersion:     26.5
BuildVersion:       25F5058e
$ xcodebuild -version
Xcode 26.5
Build version 17F42
$ clang --version
Apple clang version 21.0.0 (clang-2100.1.1.101)
Target: arm64-apple-darwin25.5.0
$ xcrun --show-sdk-version
26.5
$ xcrun --show-sdk-path
/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
```

CI runners targeted by this plan: `macos-14`, `macos-15`, `macos-latest` (whatever Xcode that resolves to at runtime — currently Xcode 16.x at the time of writing, but the audit gate is environment-agnostic).

### 3.1 LC_MAIN threshold is at min=10.8 (toolchain-confirmed)

Build experiments on the dev machine (Xcode 26.5), all using the same phracker `MacOSX10.13.sdk` at `/tmp/MacOSX10.13.sdk`, varying only the `-mmacosx-version-min` flag:

| `-mmacosx-version-min` | Entry-point | LC_VERSION_MIN_MACOSX | LC_SOURCE_VERSION | command (`x86_64`, abbreviated) |
|---|---|---|---|---|
| `10.6` | **LC_UNIXTHREAD** | `version 10.6 sdk 10.6` | No | `clang … -arch x86_64 -isysroot /tmp/MacOSX10.13.sdk -mmacosx-version-min=10.6 …` |
| `10.7` | **LC_UNIXTHREAD** | `version 10.7 sdk 10.7` | No | (same with `=10.7`) |
| `10.8` | LC_MAIN | `version 10.8` | Yes | (same with `=10.8`) |

Conclusion: the recipe `-isysroot $(LEGACY_SDK) -mmacosx-version-min=10.6` (or 10.7) for x86_64 emits `LC_UNIXTHREAD` deterministically on this toolchain. Threshold is exactly at 10.8 — one-version safety margin at 10.7.

Cross-checked: `MACOSX_DEPLOYMENT_TARGET=10.6` env var alone (without `-mmacosx-version-min`) on this toolchain → `LC_VERSION_MIN_MACOSX 10.6` + `LC_UNIXTHREAD` (env var is honoured on Xcode 26.5). On Xcode 15.5 it was not honoured (per the released v2.4.3 artifact, which has `LC_MAIN`). The Makefile must use the explicit flag, *and* CI must verify the resulting binary against the invariant.

**Source citation.** `apple-oss-distributions/ld64` `src/ld/Options.cpp` — search for `fEntryPointLoadCommand`; `src/ld/ld.hpp` — search for `version2012`. URL pattern: `https://github.com/apple-oss-distributions/ld64/blob/main/src/ld/Options.cpp` (pin to specific commit at sign-off; main branch URLs move).

### 3.2 The fixed binary runs and self-tests cleanly

The min-10.6 x86_64 binary (`/tmp/lc-test/b-explicit106`), run on the dev arm64 host via Rosetta 2:

```
$ /tmp/lc-test/b-explicit106 --version
mac-guest-agent test
$ /tmp/lc-test/b-explicit106 --self-test-json | python3 -c 'import sys,json; d=json.load(sys.stdin); print(f"passes={d[\"passes\"]} warnings={d[\"warnings\"]} errors={d[\"errors\"]} arch={d[\"system_info\"][\"arch\"]} commands={d[\"system_info\"][\"command_count\"]}")'
passes=15 warnings=5 errors=0 arch=x86_64 commands=45
```

45 commands registered (parity with v2.4.3). Self-test JSON shape unchanged. `freeze_dispatch` contract preserved. Binary size: 139,736 bytes (vs. v2.4.3's 131,616 — +8 KB for LC_UNIXTHREAD's wider thread-state record). The 5 warnings are environmental (`not running as root`, `no serial device`, `/var/log not writable`, etc.) — identical to what production reports outside a VM.

**Scope of this check:** confirms the *binary loads and registers commands on a modern host*. Does **not** confirm runtime behaviour on 10.6/10.7. That requires vit9696's hardware (or eventually a CI dry-load — see §4.5).

### 3.3 Load-command + dylib + symbol audit of the fixed binary

Three layers of static check on the min-10.6 x86_64 binary:

**Layer 1: per-command audit.**

| Load command | Numeric | LC_REQ_DYLD? | Introduced | 10.6 dyld behaviour |
|---|---|---|---|---|
| `LC_SEGMENT_64` | `0x19` | No | 10.6 | Universal |
| `LC_DYLD_INFO_ONLY` | `0x80000022` | **Yes** | 10.6 | Recognised by 10.6 dyld (Mozilla bug 602049: introduced 10.6, fails on 10.5) |
| `LC_SYMTAB` | `0x2` | No | NeXT-era | Universal |
| `LC_DYSYMTAB` | `0xb` | No | NeXT-era | Universal |
| `LC_LOAD_DYLINKER` | `0xe` | No | NeXT-era | Universal |
| `LC_UUID` | `0x1b` | No | 10.5 | Recognised |
| `LC_VERSION_MIN_MACOSX` | `0x24` | No | 10.6 | Recognised, optional info |
| `LC_UNIXTHREAD` | `0x5` | No | NeXT-era | Universal — the entry-point command we want |
| `LC_LOAD_DYLIB` | `0xc` | No | NeXT-era | Universal |
| `LC_FUNCTION_STARTS` | `0x26` | No | 10.6 | Recognised, optional info |
| `LC_DATA_IN_CODE` | `0x29` | No | 10.7 | **Unknown to 10.6 dyld but SAFE** — no LC_REQ_DYLD bit → dyld skips silently |

Zero unsafe required load commands. No `LC_MAIN`, no `LC_BUILD_VERSION`, no `LC_DYLD_CHAINED_FIXUPS`, no `LC_SOURCE_VERSION`.

**Source for the "LC_REQ_DYLD means mandatory" contract:** `apple-oss-distributions/xnu` `EXTERNAL_HEADERS/mach-o/loader.h` — search for `LC_REQ_DYLD`. The accompanying comment in that header:

> *"After MacOS X 10.1 when a new load command is added that is required to be understood by the dynamic linker for the image to execute properly the LC_REQ_DYLD bit will be or'ed into the load command constant. If the dynamic linker sees such a load command it does not understand it will issue a 'unknown load command required for execution' error and refuse to use the image."*

**Layer 2: dylib dependency check.** `otool -arch x86_64 -L b-explicit106` returns exactly three dependencies:

```
/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/usr/lib/libSystem.B.dylib
```

All three present on every macOS from 10.4 forward as system-supplied fat dylibs. No new or non-system dependency.

**Layer 3: undefined-symbol check.** `nm -arch x86_64 -u b-explicit106 | sort` returns the set of symbols our binary expects the system libraries to provide. Spot-check for symbols that were added in 10.7+: `clock_gettime` (10.12+) is absent; `os_log_create` (10.12+) is absent; `dispatch_block_create` (10.10+) is absent; `getentropy` (10.12+) is absent. The existing `.github/workflows/build.yml:88-96` `clock_gettime` gate already exists for the modern x86_64 build; we extend it to cover i386 + x86_64 of the universal in `scripts/verify-legacy-slices.sh` (§6.2).

This is the gate the audit (A2, P4, P18) demanded: load commands alone are necessary but not sufficient — a binary with right LC commands and a late symbol still fails on old dyld. Layers 2 and 3 close that hole.

### 3.4 Lipo can produce a tri-fat binary (packaging proof only)

```
$ lipo -create mga-i386 mga-amd64 mga-arm64 -output mga-trifat
$ file mga-trifat
Mach-O universal binary with 3 architectures: [i386: Mach-O executable i386]
                                                [x86_64: Mach-O 64-bit executable x86_64]
                                                [arm64: Mach-O 64-bit executable arm64]
$ ls -la mga-trifat
-rw-r--r-- ... 431544 ...
$ ./mga-trifat --version
mac-guest-agent 2.4.3
```

Size: 422 KB (i386 + x86_64 + arm64 + fat header).

**Scope.** This experiment proves:
- `lipo` accepts three slices built against different SDKs (the inputs were v2.4.3's released i386 + amd64 + arm64) and produces a valid tri-fat container.
- The fat header is well-formed.
- On the dev arm64 host, dyld correctly picks the arm64 slice and runs it.

It does **not** prove:
- The i386 slice runs on Tiger/Leopard/Snow Leopard (those slices in this experiment were already verified separately).
- The x86_64 slice runs on Snow Leopard/Lion (those have the LC_MAIN bug in the v2.4.3 inputs).
- The dyld no-fallback behaviour on old runtimes (sourced separately in §3.5).

Re-verification under the actual v2.4.4 build pipeline (new x86_64 slice built with legacy SDK + min=10.6) is required as Step 1.6 in §5.

### 3.5 dyld slice-grading behaviour (confirmed across modern and legacy dyld)

From `apple-oss-distributions/dyld` `common/MachOFile.cpp`, function `GradedArchitectures::grade()` (modern):

- arm64e/arm64 host: arm64e > arm64 > 0 (everything else)
- x86_64h host (Haswell+): x86_64h > x86_64 > 0
- x86_64 host (10.6 64-bit kernel through current Intel macOS): x86_64 > i386 > 0
- i386 host (10.4–10.6 32-bit kernel default, all 10.7 32-bit-only hardware): i386 > 0 (x86_64 ungradable)

**No slice fallback on load failure — confirmed in both modern and legacy dyld via primary source.**

| dyld version | macOS | Source citation | Behaviour |
|---|---|---|---|
| current | post-Catalina | `apple-oss-distributions/dyld/common/MachOFile.cpp` `GradedArchitectures::grade()` + `MachOAnalyzer.cpp` `fatButMissingSlice` | Picks one slice; throws/aborts on load failure; no fallback |
| 195.6 | 10.7 Lion | `apple-opensource/dyld/195.6/src/dyld.cpp` `fatFindBest()` + `src/ImageLoaderMachO.cpp` line 358-359 | `fatFindBest` picks a single slice based on cpu/subtype matching. `sniffLoadCommands` throws `dyld::throwf("unknown required load command 0x%08X", cmd->cmd)` on any unknown command with `LC_REQ_DYLD` bit. Throw propagates to process exit. **No retry-with-different-slice anywhere in the codebase.** The only "no match" path is when `fatFindBest` returns false (no architecture matches at all) — different error: `throw "no matching architecture in universal wrapper"`. |
| 132.13 | 10.6 Snow Leopard | `apple-opensource/dyld/132.13/src/ImageLoaderMachO.cpp` line 340-341, `src/dyld.cpp` line 1697 / 2420 / 3669 | Identical pattern to 195.6 — `fatFindBest()` selects one slice, `sniffLoadCommands` throws on unknown LC_REQ_DYLD, no fallback. |

Implication for universal-only distribution: a malformed slice in the universal binary that matches the host's grade-preferred architecture **will kill the process**. dyld will not try a lower-graded slice. The CI gate (`scripts/verify-legacy-slices.sh`) is therefore not just nice-to-have — it is the only thing between a bad commit and a brick on real hardware.

Practical consequence for universal-only distribution:
- Snow Leopard 10.6 with 32-bit kernel default (most consumer hardware) → dyld grades i386 highest → picks i386 slice → safe.
- Snow Leopard 10.6 with 64-bit kernel (Xserve, Mac Pro, opt-in elsewhere) → dyld may pick x86_64. With LC_UNIXTHREAD x86_64 slice from §3.1, this loads cleanly. With a bad x86_64 slice in the future (regression), it would die without falling back to i386 — and with universal-only distribution, the user has no thin-artifact escape hatch. This is exactly why the CI gate (§3.3 Layers 1–3) is the load-bearing mitigation under §4.4 / issue-driven support (§D10).

### 3.6 Per-arch coupling in the project — repo-wide audit

This section was understated in plan v1; expanded here per audit P7, P13. Files that hard-code a per-arch binary name or assume per-arch distribution:

| File | Lines | Coupling | Change in §6 |
|---|---|---|---|
| `README.md` | 20, 49-51 | Install snippet uses `darwin-amd64`; matrix lists per-arch | §6.5 — collapse to universal |
| `docs/PVE.md` | 69-70 | Install snippet | §6.5 |
| `docs/UTM.md` | 217 | "Use arm64 for arm64 VMs, amd64 for emulated" sentence | §6.5 — sentence removed |
| `docs/COMPATIBILITY.md` | 49-51 (Step 2 install snippet) | Per-arch references | §6.5 |
| `docs/RELEASE_TEMPLATE.md` | 36-39 | Per-arch table | §6.5 — replaced with one-line note |
| `.github/workflows/release.yml` | 42-55, 65-72, 78-81 | Build all + per-arch publish + per-arch body table | §6.3 — universal-only; collapsed body |
| `.github/workflows/build.yml` | 71-72, 88-96 | i386 echo, `clock_gettime` check (x86_64 only) | §6.4 — clock_gettime extended to all slices via §6.2 script |
| `scripts/build-pkg.sh` | 3-4, 24, 34-37 | Switch on arch arg; default = host arch | §6.8 — default = universal; per-arch arg kept for internal-test |
| `scripts/verify-installer.sh` | 519-547 | `check_architecture` recommends thin binary | §6.7 — recommend universal always |
| `Makefile` | 86-91 | `build-universal` = x86_64 + arm64 only (no i386) | §6.1 |
| `Makefile` | 104-108 | `dsym` — generates dSYM for x86_64 + arm64 only (no i386, no universal) | §6.1 |
| `Makefile` | 123-126 | `pkg` — `./scripts/build-pkg.sh amd64`, `arm64`, `universal` (no i386) | §6.1 — collapse to `universal` only |
| `Makefile` | 129-140 | `sign` — codesigns x86_64 + arm64 + universal (no i386) | §6.1 |
| `Makefile` | 250-256 | `dist` — copies x86_64 + arm64 + universal (no i386); creates checksums | §6.1 — collapse to universal only |
| `Makefile` | 277 | `help` — "build-universal Build x86_64+arm64 fat binary" | §6.1 — update to "i386+x86_64+arm64" |
| `src/service.c` | 149-150 | `--update` instruction text uses `mac-guest-agent-darwin-amd64` | §6.6 — collapse to universal |
| `configs/com.macos.guest-agent.plist` | — | None (Program key is path-based, not arch-based) | No change |
| `src/cmd-hardware.c` | 14-22, 79 | `host_statistics64` weak-import (per-slice resolution) | No change — weak-import is per-slice at load time, unchanged by tri-fat |

Naming standardization: all user-facing references use `mac-guest-agent-darwin-<arch>` (with `darwin-` prefix). Internal build outputs (`build/mac-guest-agent-i386`) keep their existing names; the release pipeline already renames during `Prepare release binaries`. The audit (P20) flagged 4 user-facing files using the prefix-less form (`README.md:48`, `docs/RELEASE_TEMPLATE.md:36`, `release.yml:78`, `build.yml:71-72`) — all corrected in §6.

**F02 addendum — dynamically-assembled and prose-only references found via expanded sweep:**

| File | Lines | Coupling | Change |
|---|---|---|---|
| `scripts/install.sh` | 22-28 (`detect_arch()`); 75 (`BINARY_FILE="${BINARY_NAME}-darwin-${ARCH}"`) | Assembles asset name at runtime from `uname -m`; maps i386→amd64. With universal-only, would 404 the user. | §6.6 — collapse to single `BINARY_FILE="mac-guest-agent-darwin-universal"`; drop `detect_arch()` |
| `docs/COMPATIBILITY.md` | 42 (Tiger row) | Prose "i386 binary required" — no longer accurate when the universal contains the i386 slice | §6.5 — replace with "ships in universal binary's i386 slice" or remove |

Step 5 Gate 5.1 is broadened (§5 Step 5) from grepping exact artifact-name literals to also searching for `darwin-\${ARCH}` patterns and prose-only mentions ("i386 binary", "amd64 binary", "thin binary", "per-arch download").

### 3.7 Existing universal already smoke-tested on current macOS

Current `mac-guest-agent-darwin-universal` (x86_64+arm64, no i386) installs and runs on macOS 14/15/26 — confirmed by every release since v2.4.0 and by the 10.11.6 evidence drop (38/0 PASS) shipped with v2.4.3. The universal direction is not new mechanically; we are extending it to include i386 and to fix the x86_64 slice's load command.

---

## 4. Decision & architecture

### 4.1 Chosen approach

1. **Build the x86_64 slice with `MacOSX10.13.sdk` + explicit `-mmacosx-version-min=10.6`.** (Per Step 0 outcome: 10.6 SDK was tried first but rejected due to `vm_statistics64.compressor_page_count` (10.9+) and missing `<stdint.h>` includes; 10.13 SDK + explicit min is the chosen recipe — see Step 0 details and D1 in the decision log.)
2. **Build the i386 slice with the same legacy SDK** + explicit `-mmacosx-version-min=10.4` (currently implicit; explicit-flag for auditability).
3. **Lipo i386 + x86_64 + arm64 into one tri-fat universal artifact.**
4. **Add `scripts/verify-legacy-slices.sh`** — invoked by both `.github/workflows/build.yml` and `.github/workflows/release.yml`. Fails the build if any per-slice load-command / dylib / symbol invariant is violated. **This is the load-bearing piece of the fix.**
5. **Add a hosted Linux `-m32` surrogate job** in `build.yml` that builds the genuinely-portable subset — **per H04, `protocol.c` + `cJSON.c` only** — via a new standalone `tests/surrogate_32bit_main.c` driver. Catches portable-code regressions independent of macOS hardware availability. Excluded: `log.c` and `selftest.c` (macOS deps, per §4.5/D16); `util.c` (includes compat.h, POSIX needing `_POSIX_C_SOURCE`, per H04). Base64 portability test deferred to a future ticket that splits util.c.
6. **Universal-only release.** Drop `-i386`, `-amd64`, `-arm64` thin artifacts from publication. `release.yml` publishes exactly `mac-guest-agent-darwin-universal` + `checksums.sha256`.
7. **Source / Makefile / docs sweep.** Update `src/service.c:149-150`, `Makefile` targets `dist`/`pkg`/`sign`/`dsym`/`help`, `scripts/build-pkg.sh`, `scripts/verify-installer.sh`, all docs, all release-body templates.
8. **Diagnostic addition.** Extend `--self-test-json` `system_info` with a `selected_arch` field (the slice dyld actually picked at runtime). Reports verify.sh forensics in evidence drops.
9. **Tier 1 promotion criteria respected.** Promote a macOS version to Tier 1 only when the full Tier 1 evidence set (`--self-test-json`, `--safe-test-json`, `scripts/verify.sh ALL CHECKS PASSED`, freeze cycle evidence) is collected from a tester. `--version` alone does not promote.

### 4.2 What was considered and rejected

| Alternative | Rejected because |
|---|---|
| Keep per-arch artifacts as published fallbacks | Project owner's call: universal-only release simplifies the install story to one URL and one command. Issue-driven support (§4.4 / D10) handles the rare slice-load-failure case rather than documenting a workaround. |
| Use `-Wl,-no_new_main` linker flag | Verified no-op in current ld64 (§2.4). Stack Overflow advice is a decade stale. |
| Set `MACOSX_DEPLOYMENT_TARGET` via env var only (no `-mmacosx-version-min` flag) | Toolchain-version-dependent — silently clamped on Xcode 15.5, honoured on 26.5. Explicit flag is the only environment-independent way. |
| Bump x86_64 declared min to 10.8 ("honesty") | Solves nothing for 10.6/10.7 users. We can deliver 10.6 coverage cheaply; declining is wrong. |
| Per-OS-version builders (Homebrew pattern) | Heavy CI for a small daemon. Homebrew/MacPorts ship per-arch because they're package managers; our distribution is `curl`. |
| Use a 10.7 SDK | Would work but leaves 10.6 on the table for no reason. 10.6 SDK or 10.13 SDK + min=10.6 are both strictly better. |
| `vtool --replace-load-command` post-link | Header tampering — would also need to inject crt1.o setup + thread state. Inert and misleading at best. |
| Drop x86_64 support below 10.8 | Cedes 10.6/10.7 entirely. Universal-only direction makes this strictly worse (those users now have no thin amd64 either). |
| Full Linux `-m32` build of the entire project | Significant `#ifdef` work; the high-value test surface (Mach/IOKit/launchd) cannot run on Linux. See plan v1 Q-rev-2 analysis. Option A (portable subset) chosen instead. |

### 4.3 Why min=10.6 (the decision and its boundary)

10.6 Snow Leopard is the floor of the matrix entry in `docs/COMPATIBILITY.md` (currently Tier 2). It is also the first OS X where x86_64 user-space is fully supported (10.4 had limited support; 10.5 added GUI app support). Targeting 10.6 also delivers 10.7 for free since both are pre-LC_MAIN.

**No casual fallback.** Plan v1 said "if 10.6 breaks, bump to 10.7." That was wrong. dyld on a 10.6 64-bit-kernel host (Xserve, Mac Pro) prefers x86_64 over i386 if it can load the x86_64 slice; bumping x86_64 min to 10.7 would prevent it from loading, dyld would die (no fallback to i386), and 10.6 users would be broken without recourse since there is no thin-artifact escape hatch. If a real 10.6-only issue surfaces in vit9696's testing, the response is to fix it directly or to document a narrow exclusion ("10.6 with 64-bit kernel: extract i386 slice via `lipo -thin i386`") — *not* to bump the min.

The practical exposure of the 10.6-64-bit-kernel case is bounded but nonzero: on most 10.6 hardware the kernel is 32-bit by default, but Xserve, Mac Pro, and any consumer machine where the user manually selected 64-bit-kernel boot would pick the x86_64 slice.

### 4.4 Universal-only release — rationale and recovery story

**Rationale.** Project owner's product call after reviewing plan v1: ship one binary, one URL, one install command. Removes the entire failure mode of "user downloaded wrong slice for their hardware." Aligns with industry norm (Inkscape, Audacity, Apple's own first-party tools).

**Lost recovery path.** v2.4.3 users who hit a load failure on the thin amd64 binary can `curl ... -i386` and recover manually. With v2.4.4 universal-only, that escape hatch is gone — if dyld picks a slice that won't load, the process dies before any code runs.

**Compensating mitigations baked into v2.4.4:**

1. **Strengthened CI gate** (§5 Step 3) is no longer optional; it's the only thing between us and a brick. The `scripts/verify-legacy-slices.sh` script (§6.2) enforces every slice invariant on every PR + release build. This is the primary mitigation.

2. **`selected_arch` field in `--self-test-json`.** When the agent runs, its self-test output records which slice dyld picked. Verify.sh evidence drops capture forensics — if a user later reports a load failure on host X, prior successful runs from similar hosts in evidence give us baseline data for triage.

3. **Issue-driven support model.** Per project owner's call: if a user does hit a slice-load failure, they open a GitHub issue and we work the bug. No self-help "extract the slice yourself" documentation. The fixed-set-of-OS-versions surface (10.4–26, well-enumerated) means any real failure is a fixable bug rather than a long-tail support nightmare. Cost of being wrong: if recovery cases surface frequently, we add docs/scripts in v2.4.5 — not before.

4. **Pre-release validation matrix** (§5 Step 11) covers the slice/host combinations vit9696 has access to (10.4 → i386; 10.5 → x86_64 via i386 kernel; 10.6 → x86_64; 10.11 → x86_64). If the universal works for all of those, the remaining risk window is narrow. Done post-tag (per §4.6 / Q-EXEC-5), not pre-tag.

### 4.5 Non-VM CI gates (the audit-recommended path that doesn't depend on vit9696)

Project audit A1 and plan audit P8 both flag that release readiness should not be gated solely on a community tester's availability. v2.4.4 adds two non-VM gates:

**Gate G1: static legacy-slice validation** (`scripts/verify-legacy-slices.sh`, §6.2). Runs in every CI build. Enforces:
- Architectures present: `lipo <binary> -verify_arch i386 x86_64 arm64` (binary path first; G07)
- i386 slice: `LC_UNIXTHREAD` present, no `LC_MAIN` / `LC_BUILD_VERSION` / `LC_DYLD_CHAINED_FIXUPS`, no other LC_REQ_DYLD command beyond what 10.4 dyld is documented to handle, `LC_VERSION_MIN_MACOSX 10.4`, only system dylibs in dependencies, no 10.5+ symbols.
- x86_64 slice: `LC_UNIXTHREAD` present, no `LC_MAIN` / `LC_BUILD_VERSION` / `LC_DYLD_CHAINED_FIXUPS`, only `LC_DYLD_INFO_ONLY` allowed as LC_REQ_DYLD, `LC_VERSION_MIN_MACOSX 10.6`, only system dylibs, no 10.7+ symbols (`clock_gettime`, `os_log_create`, etc.).
- arm64 slice: `LC_MAIN` present (expected for modern arm64), `LC_BUILD_VERSION sdk≥11.0` present.

**Gate G2: Linux `-m32` surrogate** (new CI job, §6.4). Builds **only the genuinely portable subset** under `gcc -m32` on `ubuntu-latest` (v2.4 H04: narrowed further):
- `src/protocol.c` — QGA JSON-RPC parsing (verified portable: `<stdlib.h>`, `<string.h>`, `cJSON.h` only)
- `src/third_party/cJSON.c` — pure portable JSON

Excluded from the surrogate (verified per H04):
- `src/util.c` — `#include "compat.h"` (macOS-specific) AND uses POSIX functions (`popen`, `fork`, `setsid`, `dup2`, `execvp`, `waitpid`) that need `_POSIX_C_SOURCE=200809L` feature-test macro on Linux glibc. Splitting util.c into a portable subset is source restructuring out of v2.4.4 scope.
- `src/selftest.c` — depends on `compat_*`, `commands_count()`, `run_command_capture` (shells to macOS tools), `cmd-fs` helpers
- `src/log.c` — uses `uint8_t`/`uint32_t` without `<stdint.h>` (same tech-debt Step 0 surfaced); also writes to `/var/log` paths
- Any `cmd-*.c` — macOS-specific (Mach, IOKit, launchd, BSD-specific)

The surrogate driver is a new, **standalone** `tests/surrogate_32bit_main.c` (not a wrapper around `test_unit.c`, which itself includes `compat.h` and links CoreFoundation — see F05). It duplicates the protocol/util/base64 portable assertions inline. Spec in §6.4.

Catches:
- 32-bit pointer-width regressions in JSON marshaling / buffer handling
- Endianness assumptions in protocol code
- Integer truncation in base64 / util
- Struct-layout regressions in shared headers (cJSON, protocol)

Does **not** catch:
- macOS-specific code paths (Mach/IOKit/launchd) — those cannot run on Linux at all
- Runtime behaviour on actual 10.4/10.5/10.6/10.7 hardware — only G3 covers that

Follow-up ticket (not v2.4.4 scope): once `<stdint.h>` is fixed in `log.c` (util.c got the fix in v2.4 per D20), once util.c is split into a portable subset that doesn't include compat.h (or once compat.h gets `#ifdef __APPLE__` gates), and once `selftest.c::emit_system_info()`'s macOS dependencies can be `#ifdef`-gated, the surrogate scope can expand. For v2.4.4 we accept the narrower `protocol.c + cJSON.c` scope — it still catches the JSON marshaling / cJSON / pointer-width regression class.

**Gate G3: vit9696 runtime validation** (§5 Step 11). Optional in the strict sense — see §4.6 for the vit9696-unavailable contingency. Where vit9696 produces full `verify.sh` evidence, Tier 1 promotion happens for that version; where he does not, the version stays at Tier 2 with a note ("v2.4.4 load command verified statically; runtime evidence pending").

### 4.6 Communication and vit9696-unavailable contingency

**Per Q-EXEC-5 decision:** No pre-announcement in issue #4. Do all the work, tag v2.4.4, then post the comprehensive Step 12 reply. The reply tells vit9696 about the universal binary, asks him to validate when convenient, and gives him a short set of diagnostic commands to run if anything goes wrong (so we don't waste a round-trip extracting basic info).

If vit9696 doesn't respond to the post-tag testing ask:

- v2.4.4 has already shipped (per the timing above); CI gates green; CHANGELOG covers the LC_MAIN fix at the static / load-command level.
- **No Tier 1 promotion claims** for 10.5/10.6/10.7 — those rows in `COMPATIBILITY.md` stay at Tier 2 with a footnote: "v2.4.4 universal binary passes static legacy-slice validation; runtime verification pending."
- If a later user hits an issue, the §4.4 issue-driven support model handles it.

This makes the release shippable without vit9696 in the critical path. Runtime evidence updates the matrix when it arrives; nothing blocks if it doesn't.

---

## 5. Implementation plan

Each step has a verification gate (`✓`). Do not advance until the gate passes.

### Step 0 — SDK selection experiment (COMPLETED 2026-05-25)

**Outcome: use `MacOSX10.13.sdk` + explicit `-mmacosx-version-min=10.6` (x86_64) and `-mmacosx-version-min=10.4` (i386).**

The 10.6 SDK was tried first per the audit's preference for stronger symbol-availability bounds. It failed to build our source with three errors:

| Error | File | Cause |
|---|---|---|
| `no member 'compressor_page_count' in 'struct vm_statistics64'` | `src/cmd-hardware.c:102` | Field added in 10.9 Mavericks; we use it unconditionally |
| `use of undeclared identifier 'SIZE_MAX'` | `src/util.c:156` | Missing `#include <stdint.h>` |
| `unknown type name 'uint8_t' / 'uint32_t'` | `src/log.c:29` | Missing `#include <stdint.h>` |

The latter two are existing technical debt (transitive includes from newer SDKs hide them); the first is a real 10.9-era API. Fixing all three would expand v2.4.4 scope into source-code changes. **Decision: stay with 10.13 SDK + explicit min flags.** Verified to produce LC_UNIXTHREAD on both legacy slices (§3.1).

SDK SHA256 pins (for CI):
- `/tmp/MacOSX10.13.sdk.tar.xz` → `1d2984acab2900c73d076fbd40750035359ee1abe1a6c61eafcd218f68923a5a`

**Brought into v2.4.4 scope (G05, retained per v2.4 H04 as hygiene):** add `#include <stdint.h>` to `util.c`. Originally added because v2.3 surrogate compiled util.c; v2.4 dropped util.c from the surrogate (H04 — compat.h + POSIX feature-tests). Include is kept anyway as one-line tech-debt cleanup: eliminates transitive-Apple-SDK reliance, paves the way for future surrogate expansion when util.c gets split. `log.c` still has the same `<stdint.h>` issue but is OUT of the surrogate scope per D16, so its include stays deferred.

**Follow-up tracked separately (not v2.4.4 scope):** add `#include <stdint.h>` to `log.c`; consider conditional-compiling `compressor_page_count` for future 10.6-SDK use.

**Step 0 also validated:** with 10.13 SDK + explicit `-mmacosx-version-min=10.4` for i386, the toolchain correctly emits **classic relocations only** (no `LC_DYLD_INFO_ONLY`). Originally tracked as R13; now resolved. The i386 slice is genuinely 10.4-dyld-safe with no extra linker flags.

### Step 1 — Local proof: full uncommitted application of all v2.4.4 changes

**Pre-condition:** `LEGACY_SDK` path from Step 0 is set; phracker 10.13 tarball present at that path.

**Action.** v2.3 fix (G03): Step 1 applies the **full v2.4.4 change set** in the working tree uncommitted. This is NOT just the Makefile change — Gates 1.2 and 1.3 reference the new verifier, the baseline files, and the `selected_arch` selftest edit, all of which only exist after the full apply. Apply locally, run gates, then `git stash` or `git checkout -- .` to revert before Step 2 commits the same set as one atomic commit.

Concrete apply set (everything from §6, in working tree, uncommitted):
- `Makefile` edits per §6.1 (including the i386 error-message URL fix per G06)
- `scripts/verify-legacy-slices.sh` per §6.2 (new file, `chmod +x`)
- `tests/legacy_slice_symbols_i386.txt` + `tests/legacy_slice_symbols_x86_64.txt` per §6.2 (new baseline files)
- `tests/surrogate_32bit_main.c` per §6.4 (new standalone driver)
- `src/util.c` `#include <stdint.h>` addition per §6.1 (one line, in v2.4.4 scope per G05)
- `src/selftest.c` `selected_arch` printf insertion per §6.9
- `src/service.c` `--update` text per §6.6
- `scripts/install.sh` per §6.5a (universal-only fetch with PPC guard per G11)
- `scripts/build-pkg.sh` per §6.8
- `scripts/verify-installer.sh` per §6.7

Then run:
```bash
make plist-header
make build-universal LEGACY_SDK=$LEGACY_SDK
./scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests
./tests/run_tests.sh build/mac-guest-agent-universal
```

**✓ Gate 1.1:** `lipo build/mac-guest-agent-universal -verify_arch i386 x86_64 arm64` exits zero. (G07 fix: binary path comes **before** `-verify_arch`, not after.)

**✓ Gate 1.2:** `scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests` exits zero (all per-slice invariants from §6.2 + symbol-baseline diff against `tests/legacy_slice_symbols_*.txt`).

**✓ Gate 1.3:** `build/mac-guest-agent-universal --version` returns the expected version string. `--self-test-json` returns valid JSON (verify with `python3 -c 'import json,sys;json.loads(sys.stdin.read())'`) with `passes ≥ 15`, `errors = 0`, `command_count: 45`, and `selected_arch` field present matching the host's arch.

**✓ Gate 1.4:** `lipo build/mac-guest-agent-universal -thin i386 -output /tmp/test-i386-only` succeeds (packaging soundness — slices are independently extractable; G07 lipo arg order corrected here too).

**✓ Gate 1.5 (G05 verification):** `make build-universal` succeeds without `<stdint.h>`-related compile errors on x86_64/i386 slices.

If any gate fails, do not proceed. Investigate and re-verify.

**Revert via explicit named stash** (H02 fix — no `git checkout -- .` which could discard unrelated tracked user work):
```bash
git stash push --include-untracked -m universal-upgrade-step1-proof
# Verify clean tree, OR if you want to inspect what was stashed: git stash show -p stash@{0}
```
Step 2 then applies the same set fresh and commits it. The stash is preserved for reference (drop with `git stash drop stash@{0}` once Step 2 is done).

**G03 fix:** v2.3 rewrote Step 1 as a full uncommitted application of all v2.4.4 changes. The revert mechanism above is the v2.4 H02 refinement — explicit named stash, no blanket checkout.

### Step 2 — Bundled commit: Makefile + workflow + `scripts/verify-legacy-slices.sh`

The audit (P10, P12, P19) correctly flagged that splitting the Makefile change from the workflow change would break PR CI: after `build-x86_64` requires `LEGACY_SDK`, both `build.yml` and `release.yml` need the SDK download moved earlier in the workflow. v2.4 H03 fix: the bundled commit lists EVERY file Step 1 proves locally — they must land atomically because the workflows invoke files (verifier, baselines, surrogate driver) that those workflows then immediately depend on.

**Files in the bundled commit (must match Step 1's apply set exactly):**

| File | Section reference | Purpose |
|---|---|---|
| `Makefile` | §6.1 | Tri-fat build, explicit min flags, dist clearing |
| `scripts/verify-legacy-slices.sh` | §6.2 | CI invariant gate (mode 0755 via `git update-index --chmod=+x`) |
| `tests/legacy_slice_symbols_i386.txt` | §6.2 | i386 baseline (147 symbols, sorted) |
| `tests/legacy_slice_symbols_x86_64.txt` | §6.2 | x86_64 baseline (147 symbols, sorted) |
| `tests/surrogate_32bit_main.c` | §6.4 | Standalone Linux -m32 test driver |
| `src/util.c` | §6.1 / D20 | Add `#include <stdint.h>` (tech-debt cleanup; was originally for surrogate, now retained as hygiene per H04) |
| `src/selftest.c` | §6.9 | `selected_arch` printf insertion |
| `src/service.c` | §6.6 | `--update` text → universal |
| `scripts/install.sh` | §6.5a | Universal-only fetch with `validate_arch()` PPC guard |
| `scripts/build-pkg.sh` | §6.8 | Default arch = universal |
| `scripts/verify-installer.sh` | §6.7 | Single recommendation |
| `.github/workflows/release.yml` | §6.3 | Universal-only release + verifier gate + test universal artifact |
| `.github/workflows/build.yml` | §6.4 | SDK download + verifier gate + -m32 surrogate job |
| `README.md` | §6.5 | Universal install + modern-machine caveat + loader-safe-first troubleshooting |
| `docs/PVE.md` | §6.5 | Install snippet → universal |
| `docs/UTM.md` | §6.5 | Drop per-arch guidance sentence |
| `docs/COMPATIBILITY.md` | §6.5 | Install snippet + Tiger row prose update |
| `docs/RELEASE_TEMPLATE.md` | §6.5 | Matrix collapse |

Commit message: `build: fix x86_64 LC_MAIN regression (#4) + tri-fat universal + CI gate`.

**✓ Gate 2.1:** `.github/workflows/build.yml` `build` job (macos-latest single runner) passes.

**✓ Gate 2.2:** `.github/workflows/build.yml` `test` job (matrix: macos-14, macos-15, macos-latest) passes — note this is the multi-runner job, separate from `build`.

**✓ Gate 2.3:** `scripts/verify-legacy-slices.sh` is called from both workflows and passes.

**✓ Gate 2.4:** Existing `make test-verify-transports` still passes (orthogonal to entry-point command).

### Step 3 — Sabotage test (deterministic, replaces v1's "rebuild without SDK")

The audit (P11) correctly flagged that "rebuild without legacy SDK" no longer sabotages on current Xcode 26.5. Use a deterministic trigger:

**Action.** Add `tests/test_legacy_slice_gate.sh` that:

1. Builds an x86_64 binary with `-mmacosx-version-min=10.8` (forces `LC_MAIN` per §3.1).
2. Lipo-combines it with the i386 and arm64 binaries.
3. Invokes `scripts/verify-legacy-slices.sh` on the result.
4. Asserts the script exits non-zero with a clear error message naming `LC_MAIN`.
5. Repeats with i386 built at `-mmacosx-version-min=10.6` — **verified deterministic** (G04: empirical re-test 2026-05-25 against 10.13 SDK confirms i386 min=10.6 emits `LC_DYLD_INFO_ONLY`; min=10.5 does NOT). Asserts verifier rejects this i386 build because the per-slice allowlist for i386 disallows `LC_DYLD_INFO_ONLY` AND because min must be 10.4.

This is a self-contained CI test — no dev machine state required.

**✓ Gate 3.1:** `tests/test_legacy_slice_gate.sh` passes (both sabotage cases correctly rejected).

**✓ Gate 3.2:** The script is wired into the existing `make test` target.

### Step 4 — Linux `-m32` surrogate job

Add a new **standalone** test driver and CI job. Per F05, we cannot reuse `tests/test_unit.c` (it includes `compat.h` and links CoreFoundation). New file:

**`tests/surrogate_32bit_main.c`** (new, sketched in §6.4):
- Has its own `main()` and assertion macros
- Tests base64 round-trip, JSON parse/print round-trip, util buffer helpers
- Includes only: `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<stdint.h>`, `"util.h"`, `"protocol.h"`, `"third_party/cJSON.h"`
- Does NOT include `compat.h` — surrogate scope is restricted to portable code (per F04)
- ~150 lines duplicating the portable assertions from `test_unit.c`'s `test_base64`, `test_protocol`, `test_util` (not `test_compat`)

CI job in `.github/workflows/build.yml`:

```yaml
  surrogate-32bit:
    name: 32-bit portable code surrogate (Linux -m32)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install 32-bit toolchain
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y gcc-multilib
      - name: Build portable subset under -m32 (excludes selftest.c, log.c, compat.c per F04)
        run: |
          gcc -m32 -Wall -Wextra -Werror -O2 -std=c99 \
              -DVERSION='"surrogate-32"' \
              -DMGA_SURROGATE_32BIT \
              -Isrc -Isrc/third_party \
              -o /tmp/mga-surrogate-32 \
              tests/surrogate_32bit_main.c \
              src/protocol.c src/util.c \
              src/third_party/cJSON.c
      - name: Run portable unit tests under 32-bit code
        run: /tmp/mga-surrogate-32
```

If `util.c` or `protocol.c` turn out to transitively include `compat.h` (verified at Step 4 build time), the fix is one of:
- Add `#ifndef MGA_SURROGATE_32BIT` guards around the compat includes
- Move the portable helpers into a separate `util_portable.c` with no compat dependency
- Drop the offending file from the surrogate scope and document the reduction

The surrogate is **blocking** per Q-EXEC-3 / D15.

**✓ Gate 4.1:** Surrogate job passes on `ubuntu-latest`.

**✓ Gate 4.2:** Deliberate regression test: introduce a `sizeof(void*) == 8`-assuming bug in `protocol.c` (the only project source the v2.4 H04-narrowed surrogate compiles besides cJSON), confirm surrogate fails; revert.

**✓ Gate 4.3:** Surrogate build successfully links *without* `compat.c`, `selftest.c`, `log.c`, or any `cmd-*.c`. If linking fails for missing portable symbols, refactor those out before merging.

**G05 fix (retained per v2.4 H04 as hygiene):** `src/util.c`'s missing `#include <stdint.h>` (used for `SIZE_MAX` at line 156) is brought into v2.4.4 scope. One-line change. Originally added in v2.3 because the surrogate compiled util.c; v2.4 dropped util.c from the surrogate (H04). Include is retained as tech-debt cleanup — eliminates transitive-Apple-SDK reliance, paves the way for the future surrogate scope expansion. See Step 1's apply set.

**H04 fix — surrogate scope narrowed AGAIN to drop util.c entirely.** Investigation confirmed util.c is heavier than the auditor stated:

- `src/util.c:1-15` uses `popen`, `pclose`, `pipe`, `fork`, `setsid`, `dup2`, `execvp`, `waitpid` (POSIX, needs `_POSIX_C_SOURCE=200809L` feature-test macro on Linux glibc with `-std=c99 -Werror`)
- `src/util.c:2` `#include "compat.h"` — compat.h is macOS-specific (likely brings IOKit/Mach types or version helpers)

Adding `-D_POSIX_C_SOURCE=200809L` would fix the POSIX hiding, but compat.h's macOS-specific surface would still break the build. Splitting util.c into a portable subset is source restructuring out of v2.4.4 scope.

**Resolution:** drop util.c from surrogate entirely. The truly portable subset becomes `protocol.c` + `cJSON.c` only:
- `src/protocol.c` — verified portable: includes `<stdlib.h>`, `<string.h>`, `"third_party/cJSON.h"` only. No util/compat/POSIX-feature-test deps.
- `src/third_party/cJSON.c` — pure portable, standard.

Test coverage we lose vs v2.3: base64 encode/decode portability check. Worth the discipline — covers a small surface; the surrogate's value (catch JSON marshaling/cJSON regressions under 32-bit) is preserved. Base64 portability becomes a follow-up ticket when source-split happens.

**Consequence for D20:** the `<stdint.h>` include in `src/util.c` is no longer strictly required for the surrogate (since surrogate doesn't compile util.c). Kept in v2.4.4 scope as **tech-debt hygiene** — eliminates transitive-include reliance, paves the way for future surrogate scope expansion when util.c gets split. One-line change, zero risk.

### Step 5 — Documentation update (after the artifact is verified tri-fat)

Per audit A1's "update docs only after artifact is actually tri-fat" — this step happens *after* Step 2's CI passes and Step 1's local verification confirms the new universal works.

Files updated:
- `README.md` (§6.5)
- `docs/PVE.md` (§6.5)
- `docs/UTM.md` (§6.5)
- `docs/COMPATIBILITY.md` (§6.5)
- `docs/RELEASE_TEMPLATE.md` (§6.5)
- `src/service.c:149-150` (§6.6)
- `scripts/verify-installer.sh` (§6.7)
- `scripts/build-pkg.sh` (§6.8)

All install snippets switch to `mac-guest-agent-darwin-universal`. README's new "If something goes wrong" section points users at GitHub issues with a list of diagnostic commands to include (per Q-EXEC-1 / D10; no self-help workaround paragraph).

**✓ Gate 5.1:** Repo-wide grep covers literal artifact names **AND** runtime-assembled patterns **AND** prose-only references (broadened per F02):
```bash
# Literal artifact names
rg -n 'darwin-amd64|darwin-i386|darwin-arm64|mac-guest-agent-i386|mac-guest-agent-amd64|mac-guest-agent-arm64' \
   --type-add 'all:*' --type all

# Runtime-assembled patterns (catches scripts/install.sh-style code)
rg -n 'darwin-\$\{?ARCH|darwin-\$\(' --type-add 'all:*' --type all

# Prose-only references that imply per-arch downloads
rg -nE 'i386 binary|amd64 binary|arm64 binary|thin binary|per-arch (download|binary)' --type-add 'all:*' --type all
```
Every remaining match is classified as one of: (a) internal Makefile build output naming, (b) intentional CHANGELOG / git history, (c) the `lipo -thin` packaging soundness gate (§5 Step 1 Gate 1.4). No match remains as a primary install instruction or a user-facing per-arch hint.

**✓ Gate 5.2:** A reviewer reads the updated `README.md` cold and concludes: "I download `mac-guest-agent-darwin-universal`, run `--install`, done."

### Step 6 — Drop thin per-arch artifacts from release pipeline

Per project owner's universal-only decision. Changes to `.github/workflows/release.yml`:

- Drop the `Prepare release binaries` step's per-arch copies for `-i386`, `-amd64`, `-arm64`. Keep only the universal copy.
- Update the `Create GitHub Release` `files:` list to publish only `mac-guest-agent-darwin-universal` + `checksums.sha256`.
- Update release body template (§6.5 / §6.3 — release.yml body) to recommend universal as the only download, with issue-link paragraph (no self-help workaround per D10).

**✓ Gate 6.1:** A dry-run workflow trigger (push to a `test/` branch with a `v2.4.4-rc.N` tag) produces a release page with exactly two assets: `mac-guest-agent-darwin-universal` and `checksums.sha256`.

**✓ Gate 6.2:** Downloaded universal passes `scripts/verify-legacy-slices.sh`.

**✓ Gate 6.3 (per F06):** Release workflow now executes the test suite against `build/mac-guest-agent-universal` (specifically the host-runnable slice via dyld's normal pick), not against `build/mac-guest-agent` (the host-arch thin build). Add a step to `release.yml` after `verify-legacy-slices.sh`:
```yaml
      - name: Run test suite against the published artifact (not a host-arch thin build)
        run: ./tests/run_tests.sh ./build/mac-guest-agent-universal
```
This guarantees the binary that ships is the binary that was tested. The previous `make build && tests/run_tests.sh ./build/mac-guest-agent` was a separate compilation of the host-arch slice — could diverge from the universal.

### Step 7 — `--self-test-json` adds `selected_arch` field (via printf, not cJSON)

Per §4.4 mitigation 3. `src/selftest.c::emit_system_info()` is pure printf (verified — see F07 in §6.9). Insert a printed field before the `"command_count"` line, preserving the trailing-comma pattern. Final form in §6.9.

Compile-time constant — the slice the binary was compiled as is the slice dyld is running. ~6 lines of code (preprocessor + printf), one new JSON field.

**✓ Gate 7.1:** Building each slice independently and running `--self-test-json` returns the matching `selected_arch`.

**✓ Gate 7.2:** Running the universal binary via `arch -x86_64 ./mac-guest-agent-darwin-universal --self-test-json` (on a Mac that supports Rosetta) returns `selected_arch: "x86_64"` despite the host being arm64 — confirms the field captures the actually-running slice.

**✓ Gate 7.3:** `python3 -c 'import json,sys; json.loads(sys.stdin.read())' < <(./mac-guest-agent --self-test-json)` succeeds — proves the printf insertion didn't break JSON validity (no missing/extra commas).

### Step 8 — Verify `--update` path with universal

`src/service.c`'s `--update` handler at lines 149-150 prints update instructions referencing `mac-guest-agent-darwin-amd64`. Update to:

```c
fprintf(stderr, "  2. scp mac-guest-agent-darwin-universal user@vm-ip:/tmp/\n");
fprintf(stderr, "  3. sudo mac-guest-agent --update /tmp/mac-guest-agent-darwin-universal\n");
```

Read the surrounding `--update` validation code end-to-end to confirm no implicit arch coupling (it should `stat`/`exec` the new binary by path; arch detection is dyld's job at exec time).

**✓ Gate 8.1:** Manual smoke test on the dev macOS 26.5: `sudo /usr/local/bin/mac-guest-agent --update /tmp/mac-guest-agent-darwin-universal` succeeds (or appropriate dry-run equivalent that exercises the code path without overwriting the running binary).

**✓ Gate 8.2:** `src/service.c` re-read confirms no arch-conditional code path in install/uninstall/update.

### Step 9 — Update CHANGELOG and COMPATIBILITY matrix (pre-tag)

Per §6.10. CHANGELOG covers: LC_MAIN fix, universal-only release, CI gate, surrogate job, source-code sweep. COMPATIBILITY matrix tier rows for 10.5/10.6/10.7 stay at Tier 2 with footnote "v2.4.4 universal binary passes static legacy-slice validation; runtime verification pending" — they get upgraded to Tier 1 in a follow-up commit only after Step 11 produces runtime evidence.

**✓ Gate 9.1:** CHANGELOG is readable and accurate — a reviewer can tell exactly what shipped without reading the PR.

**✓ Gate 9.2:** COMPATIBILITY matrix tier rows match the evidence we have at tag time (none yet for 10.5/10.6/10.7 runtime).

### Step 10 — Tag v2.4.4

**Action.**
```bash
# Bump version
sed -i '' 's/^VERSION := 2\.4\.3/VERSION := 2.4.4/' Makefile

# Regenerate manpage (CI enforces freshness; do this explicitly)
make docs/mac-guest-agent.8
git add Makefile docs/mac-guest-agent.8 CHANGELOG.md docs/COMPATIBILITY.md
git commit -m "v2.4.4: universal-only release, x86_64 LC_MAIN fix (#4)"
git tag -a v2.4.4 -m "v2.4.4"
git push origin main
git push origin v2.4.4
```

Note: VERSION is at `Makefile:2` (audit P16 corrected my v1 reference to line 5).

**✓ Gate 10.1:** Release workflow completes successfully.

**✓ Gate 10.2:** `curl -fsSL https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-universal` returns the new tri-fat (verify with `file` and `lipo -info`).

**✓ Gate 10.3:** Binary stamps as `mac-guest-agent 2.4.4`.

**✓ Gate 10.4:** Release page shows exactly 2 assets: universal + checksums.

### Step 11 — Post-tag: reply to issue #4 with diagnostic prompts

Per Q-EXEC-5 decision: this is the first communication to vit9696 about v2.4.4. Done **after** the tag, not before.

Reply structure:
1. Acknowledge the fix shipped (link to release).
2. State the install command (universal-only URL).
3. Ask him to test on 10.4.11, 10.5.8, 10.6.8 when convenient.
4. **Give him diagnostic commands to run if anything is off**, so we don't waste a round-trip on basic info-gathering:
   ```bash
   # On the VM, after install:
   sudo /usr/local/bin/mac-guest-agent --version
   sudo /usr/local/bin/mac-guest-agent --self-test-json | python -m json.tool | head -30
   # Specifically grab "selected_arch" from system_info — tells us which slice dyld picked
   file /usr/local/bin/mac-guest-agent
   lipo -info /usr/local/bin/mac-guest-agent
   # On the PVE host:
   curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh | bash -s -- <vmid> | tee verify.txt
   ```

For each version where he returns `ALL CHECKS PASSED`, promote that row to Tier 1 in `docs/COMPATIBILITY.md` (separate commit, post-tag). For versions where he can't test or doesn't return data within a reasonable window, leave at Tier 2 with the v2.4.4 footnote.

**✓ Gate 11.1:** Reply posted to issue #4 within 24 hours of tag.

**✓ Gate 11.2:** When/if vit9696 returns evidence: `docs/evidence/<version>/` updated, COMPATIBILITY matrix promoted as warranted.

**✓ Gate 11.3 (regression baseline):** The 10.11.6 evidence-drop re-run on our dev hardware still reports 38/0 PASS — this we can do ourselves, doesn't require vit9696.

Per §6.10. CHANGELOG covers: LC_MAIN fix, universal-only release, CI gate, surrogate job, source-code sweep. COMPATIBILITY matrix updates only happen for versions where Step 9 produced full Tier 1 evidence. Versions where only static gates pass get a footnote, not a tier change.

*(Steps 9 + 10 + 11 above replace the previous Step 9 / Step 10 / Step 11 / Step 12 / Step 13 — renumbered after Step 10 (dyld no-fallback experiment) was deleted because §3.5 now has primary-source confirmation from dyld-132.13 and dyld-195.6.)*

---

## 6. Concrete diffs (audit-readable)

### 6.1 Makefile — full pass (corrects audit P7, P17, expanded scope; Step 0 outcome incorporated)

```diff
# Around line 2 — VERSION bump (deferred to Step 10)

# Around line 56-67 — i386 and x86_64 build recipes
@@ -56,17 +56,21 @@
 # i386 build requires legacy SDK (Xcode 10+ dropped i386 support)
 # Download SDK: curl -L -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz && tar xf /tmp/sdk.tar.xz -C /tmp
-I386_SDK ?= /tmp/MacOSX10.13.sdk
+# Per universal_upgrade.md Step 0 (2026-05-25): 10.6 SDK fails to build our source
+# (vm_statistics64.compressor_page_count is 10.9+; util.c/log.c lack <stdint.h>).
+# Decision: 10.13 SDK + explicit min flags. Verified to emit LC_UNIXTHREAD on both legacy slices.
+LEGACY_SDK ?= /tmp/MacOSX10.13.sdk
+I386_SDK ?= $(LEGACY_SDK)
+
build-i386: plist-header
       @echo "Building $(PROGRAM_NAME) v$(VERSION) (i386, 10.4+)..."
       @if [ ! -d "$(I386_SDK)" ]; then echo "Error: i386 SDK not found at $(I386_SDK)"; echo "Download: curl -L https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz | tar xJ -C /tmp"; exit 1; fi
       @mkdir -p $(BUILD_DIR)
-       MACOSX_DEPLOYMENT_TARGET=10.4 $(CC) -Wall -Wextra -Werror -O2 -std=c99 -DVERSION=\"$(VERSION)\" \
-               -Wno-deprecated-declarations $(INCLUDES) -arch i386 -isysroot $(I386_SDK) \
+       $(CC) -Wall -Wextra -Werror -O2 -std=c99 -DVERSION=\"$(VERSION)\" -mmacosx-version-min=10.4 \
+               -Wno-deprecated-declarations $(INCLUDES) -arch i386 -isysroot $(I386_SDK) \
                -o $(BUILD_DIR)/$(PROGRAM_NAME)-i386 $(SRCS) $(LDFLAGS)
        @echo "i386 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-i386"

 # x86_64 targeting 10.6+
 build-x86_64: plist-header
-       @echo "Building $(PROGRAM_NAME) v$(VERSION) (x86_64, 10.6+)..."
+       @echo "Building $(PROGRAM_NAME) v$(VERSION) (x86_64, 10.6+, LC_UNIXTHREAD via legacy SDK)..."
+       @if [ ! -d "$(LEGACY_SDK)" ]; then echo "Error: legacy SDK not found at $(LEGACY_SDK)"; exit 1; fi
        @mkdir -p $(BUILD_DIR)
-       MACOSX_DEPLOYMENT_TARGET=10.6 $(CC) $(CFLAGS) $(INCLUDES) -arch x86_64 \
+       $(CC) $(CFLAGS) -mmacosx-version-min=10.6 \
+               -Wno-deprecated-declarations $(INCLUDES) -arch x86_64 -isysroot $(LEGACY_SDK) \
                -o $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(SRCS) $(LDFLAGS)
        @echo "x86_64 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64"

# Around line 86-91 — build-universal becomes tri-fat
@@ -85,9 +89,11 @@
 # Universal binary (x86_64 + arm64)
-build-universal: build-x86_64 build-arm64
-       @echo "Creating universal binary..."
-       lipo -create $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 \
+# v2.4.4: tri-fat with i386 + x86_64 + arm64
+build-universal: build-i386 build-x86_64 build-arm64
+       @echo "Creating tri-fat universal binary (i386 + x86_64 + arm64)..."
+       lipo -create \
+               $(BUILD_DIR)/$(PROGRAM_NAME)-i386 \
+               $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 \
+               $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 \
                -output $(BUILD_DIR)/$(PROGRAM_NAME)-universal
        @echo "Universal binary: $(BUILD_DIR)/$(PROGRAM_NAME)-universal"

# Around line 93 — build-all already depends on build-universal which now pulls i386
# No change needed — build-all: build-x86_64 build-arm64 build-universal
# After change, build-universal depends on build-i386, so build-all transitively includes it.

# Around line 104-108 — dsym now generates universal dsym
@@ -103,9 +109,7 @@
 dsym: build-all
        @echo "Generating dSYM files..."
-       @dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 -o $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64.dSYM 2>/dev/null || true
-       @dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 -o $(BUILD_DIR)/$(PROGRAM_NAME)-arm64.dSYM 2>/dev/null || true
+       @dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-universal -o $(BUILD_DIR)/$(PROGRAM_NAME)-universal.dSYM 2>/dev/null || true
        @echo "dSYM files generated"

# Around line 123-126 — pkg only builds universal
@@ -122,9 +126,7 @@
 # Build .pkg installer (double-click or sudo installer -pkg)
 pkg: build-all
-       @./scripts/build-pkg.sh amd64
-       @./scripts/build-pkg.sh arm64
        @./scripts/build-pkg.sh universal

# Around line 129-140 — sign only signs universal
@@ -128,11 +130,9 @@
 sign: build-all
        @echo "Signing binaries..."
        @if security find-identity -v -p basic 2>/dev/null | grep -q "Developer ID"; then \
                IDENTITY=$$(security find-identity -v -p basic | grep "Developer ID Application" | head -1 | awk -F'"' '{print $$2}'); \
-               codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64; \
-               codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-arm64; \
                codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-universal; \
                echo "Signed with: $$IDENTITY"; \
        else \
                echo "No Developer ID found. Install one from developer.apple.com"; \
                exit 1; \
        fi

# Around line 250-256 — dist only ships universal
@@ -249,11 +249,11 @@
 dist: build-all
        @echo "Creating distribution..."
-       @mkdir -p $(DIST_DIR)
+       @rm -rf $(DIST_DIR) && mkdir -p $(DIST_DIR)
-       @cp $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(DIST_DIR)/$(PROGRAM_NAME)-darwin-amd64
-       @cp $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 $(DIST_DIR)/$(PROGRAM_NAME)-darwin-arm64
        @cp $(BUILD_DIR)/$(PROGRAM_NAME)-universal $(DIST_DIR)/$(PROGRAM_NAME)-darwin-universal
        @cd $(DIST_DIR) && shasum -a 256 * > checksums.sha256
        @echo "Distribution ready in $(DIST_DIR)/"

# Around line 277 — help mentions tri-fat
@@ -276,7 +276,7 @@
        @echo "  build-i386      Build i386 (10.4+)"
        @echo "  build-x86_64    Build x86_64 (10.6+)"
        @echo "  build-arm64     Build arm64 (11.0+)"
-       @echo "  build-universal Build x86_64+arm64 fat binary"
+       @echo "  build-universal Build tri-fat (i386+x86_64+arm64) — the v2.4.4+ canonical artifact"
        @echo "  build-all       Build all architectures"
```

**Preserves:** `@mkdir -p $(BUILD_DIR)` in every recipe (audit P17 caught my v1 omission). `I386_SDK` defaults to `$(LEGACY_SDK)` for backward compatibility.

**Removes:** `MACOSX_DEPLOYMENT_TARGET` env var on both legacy recipes — replaced with explicit `-mmacosx-version-min`. Audit P2 / §2.4 / §3.1 explain why the env var is unreliable.

**Adds:** explicit `-mmacosx-version-min=10.4` to i386 path. v1 had it implicit; v2 makes it auditable.

**F08 fix:** the i386 build recipe's "Download SDK:" hint MUST reference the actual chosen SDK URL. v2.1's diff incorrectly carried `MacOSX10.6.sdk.tar.xz` from the pre-Step-0 draft. Corrected diff:
```diff
-# Download SDK: curl -L -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.6.sdk.tar.xz && tar xf /tmp/sdk.tar.xz -C /tmp
+# Download SDK: curl -L -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz && tar xf /tmp/sdk.tar.xz -C /tmp
```
Same correction applies to the i386 `@if [ ! -d ... ]` error message body.

**G06 fix:** v2.3 corrects the actual Makefile diff text (both the `# Download SDK:` comment AND the `@if [ ! -d ]` error-message body now reference `MacOSX10.13.sdk.tar.xz`). The standalone F08 note above is preserved for traceability.

**F09 fix:** `dist` target must clear stale outputs to prevent the universal-only release accidentally including thin artifacts from prior `make build-all` runs. Corrected:
```diff
 dist: build-all
 	@echo "Creating distribution..."
-	@mkdir -p $(DIST_DIR)
+	@rm -rf $(DIST_DIR) && mkdir -p $(DIST_DIR)
 	@cp $(BUILD_DIR)/$(PROGRAM_NAME)-universal $(DIST_DIR)/$(PROGRAM_NAME)-darwin-universal
 	@cd $(DIST_DIR) && shasum -a 256 * > checksums.sha256
 	@echo "Distribution ready in $(DIST_DIR)/"
```
This is strictly safer than the `checksum an explicit asset list` alternative — the explicit-list approach still leaves stale binaries in `$(DIST_DIR)/` confusing developers.

**H05 fix:** v2.4 corrects the primary §6.1 dist diff body above (was just `@mkdir -p $(DIST_DIR)`; now `@rm -rf $(DIST_DIR) && mkdir -p $(DIST_DIR)`). The F09 note below is preserved for traceability but the diff text matches.

### 6.2 `scripts/verify-legacy-slices.sh` — new file (v2.2: rewritten per F10/F11/F12; v2.3: G07/G08/G09 fixes layered on)

This is the load-bearing CI invariant. Three substantive changes from the v2.1 sketch:

1. **F10 fix — numeric `cmd` parsing.** `otool -l` renders unknown load commands as `cmd ?(0x80000028)`, not `cmd LC_...`. The v2.1 `awk '/cmd LC_/'` regex silently dropped them. v2.2 parses every `cmd` line including the numeric form and rejects any with the `LC_REQ_DYLD` bit (`0x80000000`) set unless explicitly allowlisted for that slice.

2. **F11 fix — symbol allowlist, not blocklist.** v2.1 blocked 5 known-late symbols; ignored everything else. v2.2 diffs the current build's `nm -u` output against a checked-in per-slice baseline file (`tests/legacy_slice_symbols_i386.txt` and `..._x86_64.txt`, sorted, ~147 lines each). New symbols fail the build until a contributor updates the baseline file + records the rationale in the PR.

3. **F12 fix — script permissions.** Committed with mode `0755` via `git update-index --chmod=+x` (or equivalent). Workflows still invoke as `./scripts/verify-legacy-slices.sh` (no `bash` prefix needed).

Baseline allowlist files are committed at Step 1, captured from the Step-0-verified clean build. Diffing approach catches both *added* (potentially late-symbol-introducing) and *removed* (might mean an API call disappeared accidentally) symbols.

Final script:

```bash
#!/bin/bash
# Verify per-slice invariants for the tri-fat universal binary.
# Called by .github/workflows/build.yml and .github/workflows/release.yml.
#
# Exit code: 0 if all invariants hold; non-zero (with ::error::) on first failure.
#
# Invariants are the contract that the v2.4.4 universal binary must satisfy
# to be safe for macOS 10.4 through current. Derived from universal_upgrade.md
# §3.3 + §6.2 + Apple's mach-o/loader.h LC_REQ_DYLD semantics.

set -euo pipefail

BIN="${1:-build/mac-guest-agent-universal}"
SYMBOL_DIR="${2:-tests}"   # location of legacy_slice_symbols_<arch>.txt
[ -f "$BIN" ] || { echo "::error::$BIN not found"; exit 1; }

fail() { echo "::error::$1"; exit 1; }
pass() { echo "  PASS  $1"; }
info() { echo "  INFO  $1"; }

# --- Helpers ------------------------------------------------------------

# Extract ALL load commands per slice, including numeric `cmd ?(0x...)` form.
# Returns one command per line: either LC_NAME or ?(0xHEX).
slice_loads_all() {
  otool -arch "$1" -l "$BIN" \
    | awk '/^[[:space:]]*cmd[[:space:]]+/{print $NF}'
}

slice_min_macosx() {
  otool -arch "$1" -l "$BIN" \
    | awk '/cmd LC_VERSION_MIN_MACOSX/{flag=1; next} flag && /version/{print $2; exit}'
}

slice_deps() { otool -arch "$1" -L "$BIN" | tail -n +2 | awk '{print $1}'; }
slice_undefs() { nm -arch "$1" -u "$BIN" 2>/dev/null | awk '{print $NF}' | sort; }

# Numeric cmd value parser. Returns 0 (true) if a command has LC_REQ_DYLD bit set.
has_req_dyld_bit() {
  case "$1" in
    "?("*) hex=$(echo "$1" | sed 's/^?(\(0x[0-9a-fA-F]*\))/\1/')
           [ $((hex & 0x80000000)) -ne 0 ] && return 0 || return 1 ;;
    *) return 1 ;;  # named LC_* commands are handled by the named-allowlist
  esac
}

# --- 1. Architecture set ------------------------------------------------
# G07 fix: lipo arg order — binary path MUST come before -verify_arch.
# `lipo -verify_arch ... PATH` fails: "unknown architecture specification flag"
# because lipo interprets the path as another arch token.
lipo "$BIN" -verify_arch i386 x86_64 arm64 \
  || fail "Universal binary missing one of [i386, x86_64, arm64]"
pass "Architectures: i386 + x86_64 + arm64"

# --- 2. Allowed system dylibs -------------------------------------------
SAFE_DEPS=(
  "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"
  "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit"
  "/usr/lib/libSystem.B.dylib"
)

# --- 3. Per-slice allowlists (G09 fix: closed-list known LCs) ------------
# Strategy: maintain a complete enumeration of ALL load commands we know about,
# plus a per-slice subset of which are permitted. Any LC name we don't recognize
# (because Apple added a new one to ld64/otool since this list was updated) is a
# hard failure — forces the contributor to review the new command and decide
# whether it's safe per the slice's dyld floor.
#
# Known LCs from apple-oss-distributions/xnu/EXTERNAL_HEADERS/mach-o/loader.h
# as of the v2.4.4 plan date (2026-05-25). Update this list when ld64 emits
# something new.
KNOWN_LCS="
LC_SEGMENT LC_SEGMENT_64 LC_SYMTAB LC_DYSYMTAB
LC_LOAD_DYLINKER LC_UUID LC_VERSION_MIN_MACOSX LC_VERSION_MIN_IPHONEOS
LC_VERSION_MIN_TVOS LC_VERSION_MIN_WATCHOS LC_BUILD_VERSION
LC_SOURCE_VERSION LC_FUNCTION_STARTS LC_DATA_IN_CODE LC_DYLIB_CODE_SIGN_DRS
LC_LINKER_OPTION LC_LINKER_OPTIMIZATION_HINT LC_NOTE LC_ENCRYPTION_INFO
LC_ENCRYPTION_INFO_64 LC_CODE_SIGNATURE LC_SEGMENT_SPLIT_INFO
LC_LOAD_DYLIB LC_ID_DYLIB LC_PREBOUND_DYLIB LC_SUB_FRAMEWORK
LC_SUB_CLIENT LC_SUB_UMBRELLA LC_SUB_LIBRARY LC_TWOLEVEL_HINTS
LC_PREBIND_CKSUM LC_THREAD LC_UNIXTHREAD LC_LOADFVMLIB LC_IDFVMLIB
LC_IDENT LC_FVMFILE LC_PREPAGE LC_ROUTINES LC_ROUTINES_64
LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS LC_TARGET_TRIPLE
LC_MAIN LC_DYLD_INFO LC_DYLD_INFO_ONLY LC_DYLD_ENVIRONMENT
LC_LOAD_WEAK_DYLIB LC_REEXPORT_DYLIB LC_LOAD_UPWARD_DYLIB LC_RPATH
LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_FILESET_ENTRY
LC_ATOM_INFO
"

# Per-slice REQ_DYLD-bit allowlist (named form). Any LC_REQ_DYLD-bit command
# emitted by otool as a name MUST appear here for that slice's floor, or fail.
# i386 floor: 10.4 dyld — NONE expected (classic relocations only)
# x86_64 floor: 10.6 dyld — LC_DYLD_INFO_ONLY only (introduced 10.6, see Mozilla 602049)
# arm64 floor: 11.0 dyld — modern set
ALLOWED_REQ_DYLD_i386=""
ALLOWED_REQ_DYLD_x86_64="LC_DYLD_INFO_ONLY"
ALLOWED_REQ_DYLD_arm64="LC_DYLD_INFO_ONLY LC_MAIN LC_BUILD_VERSION LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS"

# Subset of KNOWN_LCS that carry the LC_REQ_DYLD bit (per loader.h definitions).
# Sourced from xnu loader.h `LC_REQ_DYLD` annotations.
NAMED_REQ_DYLD_COMMANDS="LC_MAIN LC_DYLD_INFO_ONLY LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_BUILD_VERSION LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS LC_LOAD_UPWARD_DYLIB LC_LOAD_WEAK_DYLIB LC_REEXPORT_DYLIB LC_RPATH LC_FILESET_ENTRY LC_ATOM_INFO LC_TARGET_TRIPLE"

check_slice() {
  local arch="$1"
  local expected_min="$2"
  local allow_main="$3"

  local allowed_var="ALLOWED_REQ_DYLD_${arch}"
  local allowed_list="${!allowed_var}"

  local loads min deps
  loads=$(slice_loads_all "$arch")
  min=$(slice_min_macosx "$arch")
  deps=$(slice_deps "$arch")

  # 3a. No unknown numeric-cmd LC_REQ_DYLD entries (handles `?(0x...)` form)
  local unknown_req
  while IFS= read -r cmd; do
    if has_req_dyld_bit "$cmd"; then
      unknown_req="$unknown_req $cmd"
    fi
  done <<< "$loads"
  [ -z "$unknown_req" ] || fail "$arch slice has unknown numeric LC_REQ_DYLD command(s):$unknown_req"

  # 3b. G09 fix: every NAMED LC must be in KNOWN_LCS, else fail.
  #     This catches the case where Apple adds e.g. `LC_FUTURE_REQUIRED_CMD`
  #     that ld64 emits, otool knows the name, but our review hasn't
  #     classified it as REQ_DYLD or not. Forces explicit review.
  while IFS= read -r cmd; do
    case "$cmd" in
      LC_*)
        if ! echo " $KNOWN_LCS " | tr '\n' ' ' | grep -q " $cmd "; then
          fail "$arch slice has unrecognized LC command: $cmd (not in KNOWN_LCS; update script after reviewing whether it's REQ_DYLD)"
        fi
        ;;
      "?("*) ;; # already handled by 3a numeric check
      "") ;;    # empty line from awk
      *)
        fail "$arch slice has unparseable cmd line: '$cmd'"
        ;;
    esac
  done <<< "$loads"

  # 3c. Every named LC_REQ_DYLD command must be in the per-slice allowlist
  while IFS= read -r cmd; do
    for known in $NAMED_REQ_DYLD_COMMANDS; do
      if [ "$cmd" = "$known" ]; then
        # Is this command allowed for this arch?
        if ! echo " $allowed_list " | grep -q " $cmd "; then
          fail "$arch slice has disallowed required-load-command: $cmd (allowlist: '$allowed_list')"
        fi
      fi
    done
  done <<< "$loads"

  # 3c. LC_MAIN policy
  if [ "$allow_main" = "no" ]; then
    echo "$loads" | grep -qFx LC_MAIN \
      && fail "$arch slice has LC_MAIN (10.6/10.7 dyld rejects; issue #4)"
    echo "$loads" | grep -qFx LC_UNIXTHREAD \
      || fail "$arch slice missing LC_UNIXTHREAD"
  else
    echo "$loads" | grep -qFx LC_MAIN \
      || fail "$arch slice missing LC_MAIN (unexpected for modern arm64)"
  fi

  # 3d. Min version
  if [ -n "$expected_min" ]; then
    [ "$min" = "$expected_min" ] \
      || fail "$arch slice min=$min, expected $expected_min"
  fi

  # 3e. Dependency allowlist
  for dep in $deps; do
    local found=no
    for safe in "${SAFE_DEPS[@]}"; do
      [ "$dep" = "$safe" ] && found=yes && break
    done
    [ "$found" = "yes" ] \
      || fail "$arch slice has unexpected dependency: $dep"
  done

  # 3f. Per-slice undefined-symbol allowlist (F11 + G08 fix)
  # G08: for legacy slices (i386, x86_64), missing baseline is a HARD FAIL.
  # The arm64 slice is informational only (its dyld floor is 11.0 — modern
  # macOS handles new symbols gracefully; no Tiger-class deprecation risk).
  local symfile="$SYMBOL_DIR/legacy_slice_symbols_${arch}.txt"
  if [ -f "$symfile" ]; then
    local current_syms diff_out
    current_syms=$(slice_undefs "$arch")
    diff_out=$(diff <(sort "$symfile") <(echo "$current_syms") || true)
    if [ -n "$diff_out" ]; then
      echo "::error::$arch slice undefined-symbol set drifted from baseline ($symfile)"
      echo "$diff_out"
      fail "$arch slice symbol drift — update baseline if intentional + add CHANGELOG entry"
    fi
  else
    case "$arch" in
      i386|x86_64)
        fail "$arch slice baseline missing at $symfile — required for legacy slices (G08). Generate via: nm -arch $arch -u $BIN | sort > $symfile"
        ;;
      *)
        info "$arch slice no baseline at $symfile (informational; not required for modern slices)"
        ;;
    esac
  fi

  pass "$arch slice: LCs allowlisted, min=${min:-<no-min-cmd>}, deps OK, symbols match baseline"
}

# --- 4. Run per-slice checks --------------------------------------------
check_slice i386   10.4 no
check_slice x86_64 10.6 no
check_slice arm64  ""    yes   # arm64 has LC_BUILD_VERSION instead of LC_VERSION_MIN_MACOSX

echo ""
echo "All legacy-slice invariants verified for $BIN."
```

**G07 / G08 / G09 fixes (all in §6.2 verifier above):**
- **G07:** `lipo "$BIN" -verify_arch i386 x86_64 arm64` — binary path first. Verified locally: wrong-order exits 1 with `unknown architecture specification flag`; corrected-order exits 0 on valid tri-fat.
- **G08:** missing baseline file for i386 or x86_64 = hard FAIL (was INFO in v2.2). arm64 stays informational (modern dyld floor, different risk profile). Step 1's apply set + Step 2's commit set both include the baseline files explicitly.
- **G09:** verifier maintains a closed `KNOWN_LCS` enumeration of all currently-known load commands. Any LC name not in the list = hard FAIL (forces explicit review when Apple adds a new command). This means the per-slice REQ_DYLD allowlist is now backed by a complete name list, not an open-ended curation.

**Companion files committed to repo (created during Step 1):**

- `tests/legacy_slice_symbols_i386.txt` — sorted output of `nm -arch i386 -u build/mac-guest-agent-universal` from the first verified-clean build (147 symbols, captured Step 0 2026-05-25)
- `tests/legacy_slice_symbols_x86_64.txt` — same for x86_64 (147 symbols)

When future builds add a symbol, the verifier fails with a diff and a clear instruction to update the baseline. The contributor either:
- (a) Recognizes the symbol as 10.6+ (e.g., `_clock_gettime`) and fixes it via weak-import or alternate API
- (b) Recognizes the symbol as 10.4-safe (e.g., another POSIX function) and updates the baseline + CHANGELOG entry

**F12 fix — script permissions:** committed with mode `0755`. Step 2 commit message includes: `git update-index --chmod=+x scripts/verify-legacy-slices.sh`. CI workflows invoke `./scripts/verify-legacy-slices.sh` (no `bash` prefix needed).

**i386 LC_DYLD_INFO_ONLY status:** Step 0 confirmed that 10.13 SDK + explicit `-mmacosx-version-min=10.4` for i386 produces **no** `LC_DYLD_INFO_ONLY` (classic relocations only). The gate's i386 check has `ALLOWED_REQ_DYLD_i386=""` — anything would fail. If a future toolchain change emits `LC_DYLD_INFO_ONLY` for i386, the gate fails loudly and the mitigation is `-Wl,-no_dyld_info` on the i386 link line.

### 6.3 `.github/workflows/release.yml` — universal-only release

```diff
@@ -22,13 +22,17 @@
       - name: Extract version from tag
         run: echo "VERSION=${GITHUB_REF_NAME#v}" >> $GITHUB_ENV

-      - name: Build all architectures
-        run: make VERSION="$VERSION" build-all
-
-      - name: Download legacy SDK and build i386
+      - name: Download legacy SDK (used for i386 + x86_64 builds)
         run: |
-          curl -L -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz
+          # SDK choice per universal_upgrade.md Step 0 (10.6 SDK rejected; 10.13 used)
+          curl -fL -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz
+          EXPECTED_SHA256="1d2984acab2900c73d076fbd40750035359ee1abe1a6c61eafcd218f68923a5a"
+          actual=$(shasum -a 256 /tmp/sdk.tar.xz | awk '{print $1}')
+          [ "$actual" = "$EXPECTED_SHA256" ] || { echo "::error::SDK checksum mismatch: $actual != $EXPECTED_SHA256"; exit 1; }
           tar xf /tmp/sdk.tar.xz -C /tmp
-          make VERSION="$VERSION" build-i386 I386_SDK=/tmp/MacOSX10.13.sdk
+
+      - name: Build tri-fat universal
+        run: make VERSION="$VERSION" build-universal LEGACY_SDK=/tmp/MacOSX10.13.sdk
+
+      - name: Verify legacy slices (load commands, deps, symbol allowlist)
+        run: ./scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests
+
+      - name: Run test suite against the universal artifact (F06 — not the host-arch thin build)
+        run: ./tests/run_tests.sh ./build/mac-guest-agent-universal

       - name: Run test suite
         run: |
@@ -39,11 +43,7 @@
       - name: Prepare release binaries
         run: |
-          cp build/mac-guest-agent-i386 build/mac-guest-agent-darwin-i386
-          cp build/mac-guest-agent-x86_64 build/mac-guest-agent-darwin-amd64
-          cp build/mac-guest-agent-arm64 build/mac-guest-agent-darwin-arm64
           cp build/mac-guest-agent-universal build/mac-guest-agent-darwin-universal
           cd build && shasum -a 256 mac-guest-agent-darwin-* > checksums.sha256

       - name: Create GitHub Release
         uses: softprops/action-gh-release@v2
         with:
           files: |
-            build/mac-guest-agent-darwin-i386
-            build/mac-guest-agent-darwin-amd64
-            build/mac-guest-agent-darwin-arm64
             build/mac-guest-agent-darwin-universal
             build/checksums.sha256
           body: |
             ## macOS QEMU Guest Agent ${{ github.ref_name }}

             ### Setup

             **PVE host** (one-time):
             ```bash
             qm set <vmid> --agent enabled=1,type=isa
             ```

             **macOS VM**:
             ```bash
-            sudo cp mac-guest-agent-darwin-amd64 /usr/local/bin/mac-guest-agent
+            # One binary covers macOS 10.4 Tiger through 26 Tahoe (i386 + x86_64 + arm64).
+            sudo cp mac-guest-agent-darwin-universal /usr/local/bin/mac-guest-agent
             sudo chmod +x /usr/local/bin/mac-guest-agent
             sudo /usr/local/bin/mac-guest-agent --install
             ```

-            ### Downloads
-
-            | Binary | Arch | Min macOS |
-            |---|---|---|
-            | `mac-guest-agent-i386` | i386 | 10.4 Tiger |
-            | `mac-guest-agent-darwin-amd64` | x86_64 | 10.6 Snow Leopard |
-            | `mac-guest-agent-darwin-arm64` | arm64 | 11.0 Big Sur |
+            ### Download
+
+            Single download: `mac-guest-agent-darwin-universal` (i386 + x86_64 + arm64, covers macOS 10.4 → 26).
+
+            **If something goes wrong**, open an issue at https://github.com/mav2287/mac-guest-agent/issues/new with the following (loader-safe items 1-3 work even when the binary won't launch; 4-6 only if the binary starts):
+            1. `sw_vers` — macOS version
+            2. `file /usr/local/bin/mac-guest-agent` — Mach-O fat header
+            3. `lipo -info /usr/local/bin/mac-guest-agent` — slice list
+            4. (if it starts) `mac-guest-agent --version`
+            5. (if it starts) `mac-guest-agent --self-test-json` — note `system_info.selected_arch` in the output
+            6. (if it starts) `tail -50 /var/log/mac-guest-agent.log`
```

**H06 fix:** v2.4 propagates F13's loader-safe-first diagnostic order from README (§6.5) to release body (§6.3 above). Items 1-3 are forensics-capturable when dyld kills the process before any C code runs; 4-6 only requested when the binary actually starts.

### 6.4 `.github/workflows/build.yml` — add SDK download + tri-fat + gate + surrogate

```diff
@@ -14,6 +14,12 @@
   build:
     runs-on: macos-latest
     steps:
       - uses: actions/checkout@v4

+      - name: Download legacy SDK (i386 + x86_64 builds)
+        run: |
+          # SDK choice per universal_upgrade.md Step 0 (10.6 SDK rejected; 10.13 used)
+          curl -fL -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz
+          EXPECTED_SHA256="1d2984acab2900c73d076fbd40750035359ee1abe1a6c61eafcd218f68923a5a"
+          actual=$(shasum -a 256 /tmp/sdk.tar.xz | awk '{print $1}')
+          [ "$actual" = "$EXPECTED_SHA256" ] || { echo "::error::SDK checksum mismatch: $actual != $EXPECTED_SHA256"; exit 1; }
+          tar xf /tmp/sdk.tar.xz -C /tmp
+
       - name: Manpage freshness (regenerate from template, fail on diff)
         run: |
           # … existing content unchanged …

       - name: Static analysis (clang --analyze)
         run: |
           # … existing content unchanged …

@@ -67,12 +73,16 @@
       - name: Build all architectures
-        run: make build-all
+        run: make build-all LEGACY_SDK=/tmp/MacOSX10.13.sdk

       - name: Show binary metadata (informational)
         run: |
-          file build/mac-guest-agent-i386
-          echo "Size: $(du -h build/mac-guest-agent-i386 | cut -f1)"
+          file build/mac-guest-agent-universal
+          echo "Size: $(du -h build/mac-guest-agent-universal | cut -f1)"
+          lipo -info build/mac-guest-agent-universal

-      - name: Verify no clock_gettime (breaks pre-10.12)
-        run: |
-          for bin in build/mac-guest-agent-i386 build/mac-guest-agent-x86_64; do
-            if nm -u "$bin" 2>/dev/null | grep -q clock_gettime; then
-              echo "FAIL: $bin contains clock_gettime"
-              exit 1
-            fi
-          done
-          echo "OK: No clock_gettime references"
+      - name: Verify legacy slices (universal binary load-command / dep / symbol invariants)
+        run: ./scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests

       # … rest of build job unchanged …

# After the existing test job, add:
  surrogate-32bit:
    name: 32-bit portable code surrogate (Linux -m32)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install 32-bit toolchain
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y gcc-multilib
      - name: Build portable subset under -m32
        run: |
          gcc -m32 -Wall -Wextra -Werror -O2 -std=c99 \
            -DVERSION='"surrogate-32"' \
            -DMGA_SURROGATE_32BIT \
            -Isrc -Isrc/third_party \
            -o /tmp/mga-surrogate-32 \
            tests/surrogate_32bit_main.c \
            src/protocol.c \
            src/third_party/cJSON.c
      - name: Run portable unit tests under 32-bit code
        run: /tmp/mga-surrogate-32
```

**H04 fix (replaces G10):** v2.4 narrows surrogate scope further — drops `src/util.c` because it `#include`s `compat.h` (macOS-specific) and uses POSIX functions (`popen`/`fork`/`setsid`/`dup2`/`execvp`/`waitpid`) that need `_POSIX_C_SOURCE=200809L` to compile under `gcc -std=c99 -Werror` on Linux. Splitting util.c is out of v2.4.4 scope. Compile line is now `protocol.c + cJSON.c` only.

### 6.5 README.md / docs install snippet (F14 + F13 corrected)

Single unified pattern across `README.md`, `docs/PVE.md`, `docs/UTM.md`, `docs/COMPATIBILITY.md`, `docs/RELEASE_TEMPLATE.md`. **F14 fix:** preserve the "from a modern machine" caveat — old macOS guests (Tiger / Leopard / often Snow Leopard) have TLS stacks that can't negotiate GitHub's current certs.

```diff
-# Download binary (from a modern machine if VM can't reach GitHub)
-curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-amd64
+# Download universal binary — one slice (i386 / x86_64 / arm64) loads at runtime.
+# Covers macOS 10.4 Tiger through 26 Tahoe.
+# Note: on Tiger / Leopard / older Snow Leopard guests, the VM's TLS stack
+# usually cannot reach GitHub directly. Download on a modern machine and
+# transfer the file (scp / shared folder / USB).
+curl -L -o mac-guest-agent https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-universal
```

`docs/PVE.md` already has the longer explanation of TLS incompatibility (line 65, unchanged by this plan). README's shorter inline note above is the user's first signal.

`docs/COMPATIBILITY.md:42` Tiger-row prose says "i386 binary required" (per F02). Replace with: "Universal binary ships an i386 slice for Tiger; dyld picks it automatically. (Pre-Catalina hardware that supports x86_64 may pick x86_64 if a 64-bit kernel is booted.)"

Matrix table in README/RELEASE_TEMPLATE collapsed to a single sentence:

```
**Single download:** `mac-guest-agent-darwin-universal` covers all supported macOS versions and architectures.
```

New `README.md` "If something goes wrong" section. Per F13: loader-safe commands first (the highest-risk failure mode is dyld dying before any C code runs, so don't lead with `--self-test-json`); per F13 also: avoid `python -m json.tool` because 10.4/10.5 don't have a reliable stdlib `json` module.

```markdown
## If the agent doesn't start

Open an issue at https://github.com/mav2287/mac-guest-agent/issues/new with as much of the following as you can collect. Items 1-3 work even when the binary won't launch (dyld rejection); items 4-5 require the binary to start successfully.

**Loader-safe (no execution needed):**

1. `sw_vers` — macOS version, build
2. `file /usr/local/bin/mac-guest-agent` — confirms it's a Mach-O fat binary
3. `lipo -info /usr/local/bin/mac-guest-agent` — shows the slice list

**If the binary starts:**

4. `mac-guest-agent --version`
5. `mac-guest-agent --self-test-json` — pipe to file or `grep selected_arch` directly (do NOT pipe through `python -m json.tool` on Tiger/Leopard — those versions lack a reliable stdlib `json` module)
6. `tail -50 /var/log/mac-guest-agent.log`

We work each report as a bug — the universal binary is the supported install path; if it doesn't load on your specific host configuration we want to fix it, not work around it.
```

`docs/UTM.md:217` ("For arm64 VMs, use the arm64 binary. For emulated x86_64 VMs, use amd64.") is **deleted** entirely — the universal handles both.

### 6.5a scripts/install.sh — universal-only fetch (F02 fix)

Current script (excerpt, lines 22-28 + 75) detects host arch and assembles `mac-guest-agent-darwin-${ARCH}`. With universal-only this would 404 — `mac-guest-agent-darwin-amd64` no longer exists as a published asset.

```diff
@@ -20,12 +20,15 @@
-detect_arch() {
-    case "$(uname -m)" in
-        x86_64)  echo "amd64" ;;
-        i386)    echo "amd64" ;;
-        arm64)   echo "arm64" ;;
-        *)       err "Unsupported architecture: $(uname -m)"; exit 1 ;;
-    esac
-}
+# v2.4.4: universal-only release — no host-arch detection needed.
+# The universal binary contains i386, x86_64, and arm64 slices; dyld picks
+# the right one at load time. Single asset URL works for every supported macOS.
+#
+# G11 fix: we still validate the host arch is one we ship a slice for, so a
+# PowerPC Tiger/Leopard guest fails early with a clear message instead of
+# downloading a universal that dyld cannot load.
+validate_arch() {
+    case "$(uname -m)" in
+        x86_64|i386|i486|i586|i686|arm64|arm64e) return 0 ;;
+        *) err "Unsupported architecture: $(uname -m). This project ships i386, x86_64, and arm64 slices only. PowerPC and other architectures are not supported."; exit 1 ;;
+    esac
+}
@@ -50,8 +53,8 @@
-    ARCH=$(detect_arch)
-    info "Architecture: $ARCH"
+    validate_arch
+    info "Installing universal binary (covers i386 / x86_64 / arm64 — dyld picks at load time on $(uname -m))"
@@ -73,7 +76,7 @@
-        BINARY_FILE="${BINARY_NAME}-darwin-${ARCH}"
+        BINARY_FILE="${BINARY_NAME}-darwin-universal"
```

The error message in `curl … || { err "Download failed. On older macOS, download from another machine and use: sudo $0 --local"; exit 1; }` (line 79) is preserved verbatim — it's the F14 caveat in script form, still accurate.

**✓ Gate 5a.1:** `bash scripts/install.sh` on this dev arm64 macOS fetches `mac-guest-agent-darwin-universal` successfully and runs `--install`.

**G11 fix:** v2.3 preserves the architecture-validation guard (renamed `validate_arch`), just removes its asset-name-selection role. PowerPC and other unsupported hosts fail early with a clear message.

### 6.6 src/service.c — fix `--update` instruction text

```diff
@@ -146,9 +146,9 @@
         fprintf(stderr, "Error: provide path to new binary\n");
         fprintf(stderr, "Usage: sudo mac-guest-agent --update /path/to/new/binary\n");
         fprintf(stderr, "\nTo update from another machine:\n");
         fprintf(stderr, "  1. Download the new binary on a machine with internet\n");
-        fprintf(stderr, "  2. scp mac-guest-agent-darwin-amd64 user@vm-ip:/tmp/\n");
-        fprintf(stderr, "  3. sudo mac-guest-agent --update /tmp/mac-guest-agent-darwin-amd64\n");
+        fprintf(stderr, "  2. scp mac-guest-agent-darwin-universal user@vm-ip:/tmp/\n");
+        fprintf(stderr, "  3. sudo mac-guest-agent --update /tmp/mac-guest-agent-darwin-universal\n");
         return 1;
     }
```

### 6.7 scripts/verify-installer.sh — single recommendation

```diff
@@ -540,11 +540,9 @@
 check_architecture() {
     # … existing detection logic unchanged …

-    # Recommend the right binary
-    case "$arch_id" in
-        arm64*) info "Agent binary: arm64 (mac-guest-agent-darwin-arm64)" ;;
-        x86_64*) info "Agent binary: x86_64 (mac-guest-agent-darwin-amd64)" ;;
-        i386*)  info "Agent binary: i386 (mac-guest-agent-i386, if available)" ;;
-    esac
+    info "Agent binary: mac-guest-agent-darwin-universal (covers all macOS versions and architectures, one slice loads at runtime)"
 }
```

### 6.8 scripts/build-pkg.sh — universal default + collapsed

```diff
@@ -1,4 +1,4 @@
 #!/bin/bash
-# Usage: ./scripts/build-pkg.sh [arch]
-#   arch: amd64, arm64, or universal (default: current arch)
+# Usage: ./scripts/build-pkg.sh [arch]
+#   arch: universal (default), amd64, arm64, or i386 (legacy single-slice builds for testing only)
@@ -22,7 +22,7 @@
-ARCH="${1:-$(uname -m | sed 's/x86_64/amd64/;s/arm64/arm64/')}"
+ARCH="${1:-universal}"
@@ -32,9 +32,10 @@
 case "$ARCH" in
+    universal) BINARY="build/mac-guest-agent-universal" ;;
     amd64)  BINARY="build/mac-guest-agent-x86_64" ;;
     arm64)  BINARY="build/mac-guest-agent-arm64" ;;
-    universal) BINARY="build/mac-guest-agent-universal" ;;
+    i386)   BINARY="build/mac-guest-agent-i386" ;;
     *) echo "Unknown arch: $ARCH"; exit 1 ;;
```

### 6.9 src/selftest.c — `selected_arch` field (F07 fix: printf form, not cJSON)

`emit_system_info()` (verified: `src/selftest.c:444`) emits JSON via `printf` directly — there is no `cJSON *system_info` variable in scope. v2.1's `cJSON_AddStringToObject` proposal would not compile.

Insertion point: immediately before the existing `printf("\"command_count\":%d", commands_count());` line (the last field in `system_info`). The existing pattern uses trailing comma on every field except the last; the `selected_arch` insertion goes before `command_count` and **adds the trailing comma**, while `command_count` keeps its no-trailing-comma terminal position.

```diff
@@ src/selftest.c::emit_system_info (around line 519, before "command_count") @@
+    /* Which slice of the universal binary is actually running.
+     * Compile-time constant — set per slice during the tri-fat build. */
+    const char *selected_arch =
+#if defined(__i386__)
+        "i386"
+#elif defined(__x86_64__)
+        "x86_64"
+#elif defined(__arm64__)
+        "arm64"
+#else
+        "unknown"
+#endif
+        ;
+    printf("\"selected_arch\":\"%s\",", selected_arch);
+
     /* Command count */
     printf("\"command_count\":%d", commands_count());
```

The existing `"arch"` field (line 456) reports the *running host's* arch (via `uname -m`); the new `selected_arch` reports the *binary slice's* arch (via `__i386__` / `__x86_64__` / `__arm64__` compile-time macros). On native runs they match; on Rosetta-translated x86_64-on-arm64 runs they differ — diagnostic value comes from being able to tell which.

### 6.10 CHANGELOG entry

```markdown
## v2.4.4 (unreleased)

### Bug Fixes
- **Fixed (compatibility):** `mac-guest-agent-darwin-amd64` v2.4.3 crashed at startup on Mac OS X 10.6 Snow Leopard and 10.7 Lion with `dyld: unknown required load command 0x80000028` (SIGTRAP). The amd64 binary advertised `LC_VERSION_MIN_MACOSX 10.6` but its entry-point load command was `LC_MAIN` (introduced 10.8). The v2.4.3 release pipeline was running on a GitHub Actions runner image carrying Xcode 15.5, which silently clamped the Makefile's `MACOSX_DEPLOYMENT_TARGET=10.6` env var and emitted `LC_MAIN` regardless. Reported by @vit9696 in #4. Fixed by building both legacy slices (i386 + x86_64) against the phracker `MacOSX10.13.sdk` with explicit `-mmacosx-version-min` flags (10.4 for i386, 10.6 for x86_64), so ld64's entry-point gate resolves to false and emits `LC_UNIXTHREAD`. (The 10.6 SDK was tried first per audit guidance but rejected because our source uses `vm_statistics64.compressor_page_count`, a 10.9+ field, plus has missing `<stdint.h>` includes — see universal_upgrade.md Step 0.) Also added `scripts/verify-legacy-slices.sh` invoked by both build and release CI workflows, which fails the build on any disallowed load command, off-spec deployment target, unexpected dylib dependency, or symbol drift outside the checked-in per-slice baseline — making the invariant permanent regardless of future toolchain behaviour.

### Tooling / Packaging
- **Changed (release):** v2.4.4 publishes a **single binary**: `mac-guest-agent-darwin-universal`, a tri-fat Mach-O containing `i386 + x86_64 + arm64` slices. dyld picks the appropriate slice at load time: Tiger/Leopard pick i386; Snow Leopard through Catalina pick x86_64 (with the LC_UNIXTHREAD fix above); Big Sur and Apple Silicon pick arm64. **The thin per-arch binaries (`-i386`, `-amd64`, `-arm64`) are no longer published.** One download URL covers all supported macOS versions and architectures. If the universal doesn't start on a specific host, open an issue at https://github.com/mav2287/mac-guest-agent/issues/new — we work each report as a bug.
- **Changed:** Install URL changed from `mac-guest-agent-darwin-amd64` to `mac-guest-agent-darwin-universal`. Scripts pinning the old URL must update.
- **Added:** `scripts/verify-legacy-slices.sh` — CI-callable script that audits per-slice invariants (LC commands, deployment targets, dylib deps, undefined symbols) of the produced universal. Replaces the previous inline `clock_gettime` check; now runs against all three slices and covers more failure modes.
- **Added:** New `surrogate-32bit` CI job builds the portable subset (`protocol.c`, `util.c`, `cJSON.c`) under `gcc -m32` on `ubuntu-latest` via a standalone `tests/surrogate_32bit_main.c` driver and runs portable unit tests under 32-bit code. `selftest.c` and `log.c` are excluded because they drag macOS-specific dependencies (`compat_*`, `run_command_capture`, missing `<stdint.h>` respectively). Catches int-width / struct-layout / endianness regressions in portable code without depending on access to old Intel Mac hardware.
- **Added:** `#include <stdint.h>` to `src/util.c` (one line, no behavior change) — `SIZE_MAX` was previously visible only via transitive Apple SDK includes; the surrogate's `gcc -m32 -std=c99 -Werror` on Linux requires the explicit include.
- **Changed:** `Makefile` `build-x86_64` now uses explicit `-mmacosx-version-min=10.6 -isysroot $(LEGACY_SDK)` instead of relying on `MACOSX_DEPLOYMENT_TARGET=10.6` env var (which is toolchain-version-dependent and was the underlying mechanism of the v2.4.3 bug). `build-i386` similarly gets explicit `-mmacosx-version-min=10.4`. `LEGACY_SDK` defaults to `/tmp/MacOSX10.13.sdk` (phracker tarball, SHA256 `1d2984ac…23a5a` pinned in CI); `I386_SDK` aliases it for backward compatibility.
- **Changed:** `Makefile` `build-universal` now produces a **tri-fat** binary (i386 + x86_64 + arm64; previously x86_64 + arm64 only).
- **Changed:** `Makefile` `dist` / `pkg` / `sign` / `dsym` / `help` targets all updated for universal-only distribution.
- **Changed:** `src/service.c` `--update` flag's instruction text now references `mac-guest-agent-darwin-universal`.
- **Added:** `--self-test-json` `system_info` block now includes a `selected_arch` field reporting which slice of the universal binary dyld actually picked. Useful for verify.sh evidence drops and post-incident forensics.

### Documentation
- **Updated:** `README.md`, `docs/PVE.md`, `docs/UTM.md`, `docs/COMPATIBILITY.md`, `docs/RELEASE_TEMPLATE.md` install snippets all reference the universal binary as the single download. README's new "If something goes wrong" section asks users to open a GitHub issue with diagnostic outputs (`sw_vers`, `--self-test-json`'s `selected_arch`, agent log tail) — issue-driven support model, no self-help workaround documentation.
- **Updated:** `scripts/verify-installer.sh` recommendation collapsed from per-arch to universal.
```

**G12 fix:** v2.3 CHANGELOG entry above now describes the actual narrowed surrogate scope (`protocol.c`, `util.c`, `cJSON.c`) and discloses the `<stdint.h>` source change.

(Tier 1 promotion section to be added based on Step 11 outcome.)

---

## 7. Verification & acceptance

### 7.1 Pre-merge gates (all green required)

- [ ] Step 0 SDK selection experiment complete; chosen SDK documented in §6.1 alongside the Makefile diff.
- [ ] Step 1 gates 1.1–1.5 pass locally (H07: 1.5 added in v2.3 for G05 stdint verification).
- [ ] Step 2 gates 2.1–2.4 pass in CI (`build` and `test` jobs both green; `scripts/verify-legacy-slices.sh` runs and passes).
- [ ] Step 3 sabotage test passes (deliberate `LC_MAIN` regression rejected by gate).
- [ ] Step 4 surrogate job passes.
- [ ] Step 5 doc gates 5.1, 5.2 pass.
- [ ] Step 6 dry-run release produces exactly 2 assets.
- [ ] Step 7 `selected_arch` field present and correct.
- [ ] Step 8 `--update` smoke test passes.
- [ ] **Step 1 gate 1.5 (H07 fix)** — `<stdint.h>`/build verification passes; util.c compiles without warnings under the new include.

### 7.2 Pre-release gates

- [ ] CHANGELOG reviewed.
- [ ] COMPATIBILITY matrix updated (10.5/10.6/10.7 stay Tier 2 with v2.4.4 footnote; promotion happens post-tag if Step 11 returns evidence).

### 7.3 Post-release gates

- [ ] Step 10 gates pass (release workflow + artifact published).
- [ ] Step 11 reply posted to issue #4 within 24 hours of tag.
- [ ] Within 7 days: no new install-failure issue against v2.4.4 on macOS 10.4 → 26.
- [ ] vit9696 evidence (if collected) committed to `docs/evidence/<version>/` in a follow-up commit; matrix promotions follow.

### 7.4 Success criteria — objective

- `curl -fsSL .../releases/latest/download/mac-guest-agent-darwin-universal` returns a tri-fat with i386 + x86_64 + arm64 slices, all passing `scripts/verify-legacy-slices.sh`.
- Binary stamps as `mac-guest-agent 2.4.4`.
- The 10.11.6 evidence-drop re-run on dev hardware still reports 38/0 PASS.
- Issue #4 reply posted with universal install command + diagnostic commands for vit9696.
- (Post-vit9696 if/when available): 10.4.11, 10.5.8, 10.6.8 each report `ALL CHECKS PASSED` via `scripts/verify.sh`; matrix promoted accordingly.

---

## 8. Rollback plan

Universal-only distribution removes the per-arch escape hatch, so rollback planning matters more than in v1.

### Detection

- **Pre-tag, pre-release-workflow.** Catch via §7.1/§7.2 gates. Block tag if any fail.
- **Post-tag, post-release-workflow.** Watch for:
  - User-filed issue against v2.4.4 within 7 days alleging install failure on previously-working hardware.
  - vit9696's post-release confirmation (if Step 9 deferred to post-tag).
  - Failed CI on `main` after the tag.

### Recovery procedures

**R-0: Maintainer-only triage tool (NOT a user-facing recovery path).** Per D10 / §4.4 / §11 / Communication: there is no self-help recovery documentation. R-0 is recorded here only as an internal maintainer tool — if a user files an issue and we need to give them a one-off workaround while preparing R-1, the maintainer can manually email/comment the appropriate single-arch slice extracted via:

```bash
# Maintainer-only — DO NOT add to public README/docs
lipo -thin <arch> mac-guest-agent-darwin-universal -output mac-guest-agent-<arch>
```

For most reported issues, R-1 (next-version fix) is the response, not R-0.

**R-1: Single-bug fix release (v2.4.5).** If a defined-and-reproducible issue surfaces:
- Identify root cause from the user's `--self-test-json` `selected_arch` + `system_info.os_version` output.
- Patch in `main`, run full §7.1 + §7.2 gates.
- Tag v2.4.5.
- v2.4.5 release body references the v2.4.4 → v2.4.5 specific fix.

**R-2: Universal-only revert (v2.5.0 — only if multiple unfixable issues surface).** If universal-only proves untenable:
- Re-enable thin-artifact publication in `release.yml` (revert §6.3 `Prepare release binaries` and `Create GitHub Release` files-list changes).
- Re-add per-arch matrix in README / RELEASE_TEMPLATE / release body.
- Restore `scripts/build-pkg.sh` per-arch default.
- Tag as v2.5.0 (significant distribution-shape change).
- The universal binary continues to be published; thin artifacts return as additional options.

R-2 is a contingency, not a planned path. Expected outcome of v2.4.4: universal-only succeeds and is stable.

### Communication

- v2.4.4 release body and issue #4 reply both prominently feature the "open an issue if it doesn't start" pointer + the diagnostic commands list. No self-help workaround.
- If R-1 or R-2 invoked, post to issue #4 thread + the offending user's issue with the new install command.
- Avoid taking down v2.4.4's release — leave it published with a "see v2.4.5 for fix" note prepended to the body.

---

## 9. Risk register

| # | Risk | P | I | Score | Mitigation | Detection |
|---|---|---|---|---|---|---|
| R1 | Legacy 10.6 SDK lacks a symbol our x86_64 build needs | **RESOLVED** (Step 0) — confirmed; using 10.13 SDK | — | — | Step 0 done | — |
| R2 | phracker SDK tarball disappears | Low | High | 4 | (a) checksum-pinned in CI to `1d2984ac…23a5a` for 10.13 SDK; (b) `LEGACY_SDK` is overridable for user-supplied SDK; (c) **do NOT mirror** without legal review (audit P22) | CI build fails downloading or checksum mismatch |
| R3 | dyld no-fallback bites on 10.6 64-bit kernel — chosen x86_64 slice fails for some unforeseen reason; no thin artifact for user to fall back to (universal-only) | Low | **High** (raised from v1: no thin artifact = harder recovery) | 4 | (a) `scripts/verify-legacy-slices.sh` catches structural regressions pre-release — this is the primary mitigation; (b) `selected_arch` in self-test enables forensic triage; (c) Step 11 post-tag vit9696 ask validates on real hardware; (d) issue-driven support model (no self-help docs) means bugs become tracked work items rather than long-tail silent failures | Issue filed by user; CI gate failure pre-release |
| R4 | The CI audit gate is too strict and fails a legitimate future change (e.g., arm64 wants chained fixups) | Med | Low | 2 | Gate is per-slice; arm64 already allows chained fixups. The script is small, well-commented, and changes are visible in PR | CI fails with our own clear error |
| R5 | A 10.6-only dyld/symbol issue surfaces in vit9696 testing | Low | Med | 3 | Fix directly (do **not** bump min to 10.7 — see §4.3); or document narrow exclusion. Hold tier promotion for 10.6 if unfixable. | vit9696 reports |
| R6 | Existing v2.4.3 installs break when upgrading (URL changed from -amd64 to -universal) | Med (raised from v1: URL changed) | Low | 3 | Release notes explicitly call out URL change. `--update` flag still works locally regardless of source URL. Old URL still returns the v2.4.3 binary (GitHub Releases doesn't delete old assets). | User issue post-tag |
| R7 | Universal binary triggers Gatekeeper / quarantine surprise on Sonoma+ | Low | Low | 1 | We don't ship signed binaries; `curl` doesn't set quarantine xattr for CLI tools. Dev test on macOS 26.5 catches. | Local dev test |
| R8 | We promote 10.6/10.7 to Tier 1 from a single tester; another user reports failure | Med | Low | 2 | Tier 1 promotion only on full verify.sh evidence. CHANGELOG footnote acknowledges single-tester source. | User issue |
| R9 | Legacy SDK download adds 60–90 s to every CI build | Med | Very Low | 1 | Cache via `actions/cache` keyed on tarball SHA. Saves ~90 s per cache hit. | CI duration |
| R10 | `--install` / `--update` has implicit arch coupling we missed | Very Low | Med | 2 | Step 8 reads `src/service.c` end-to-end; smoke test on dev macOS | Smoke test catches |
| R11 | A reviewer of this plan identifies a gap we missed | Med | Med | 4 | This is precisely the §13 sign-off purpose | Reviewer comments |
| R12 | Apple's future lipo / dyld changes invalidate tri-fat | Very Low | High | 4 | Pin runner image if needed; surrogate gates surface most cases | CI failures after runner image update |
| R13 | i386 slice gets `LC_DYLD_INFO_ONLY` from the legacy SDK by default (rejected by 10.4 dyld) | **RESOLVED** (Step 0) — confirmed: 10.13 SDK + explicit `-mmacosx-version-min=10.4` emits classic relocations only | — | — | Step 0 verified | — |
| R14 | Linux `-m32` surrogate produces false positives (test passes on Linux but real macOS binary regresses) | Low | Low | 1 | Surrogate explicitly scoped to portable code. macOS-specific code paths are not surrogate-tested by design. | Discrepancy between surrogate + macOS test results |
| R15 | Audit P22 SDK redistribution: someone files a license complaint against the project | Very Low | Med | 2 | We do not redistribute the SDK; we reference phracker's published tarball | No mirroring happens |
| R16 | The release URL change breaks a downstream consumer we don't know about (Ansible role, Homebrew tap) | Low | Med | 3 | Search GitHub for `mac-guest-agent-darwin-amd64` references before tag; if found, give notice. Old URL keeps returning v2.4.3 binary so unscripted users get a slower-than-expected upgrade but no break. | Pre-tag search; user issue post-tag |

**Top concerns to monitor proactively before §5 Step 1:**
- R2 (SDK availability): checksum is pinned per §6.3/§6.4.
- R3 (slice load failure with no fallback): the `scripts/verify-legacy-slices.sh` gate is the load-bearing mitigation — its robustness directly determines our R3 exposure.
- R6 + R16 (URL change blast radius): grep before tagging.

---

## 10. Decision log

| # | Decision | Reasoning | Reviewer should challenge if … |
|---|---|---|---|
| D1 | Use 10.13 SDK + explicit `-mmacosx-version-min` flags (final, post-Step-0) | Step 0 (2026-05-25) found 10.6 SDK fails to build our source: `vm_statistics64.compressor_page_count` missing (10.9+ field) plus our own missing `<stdint.h>` includes in util.c + log.c. 10.13 SDK + explicit min works (verified §3.1) and produces LC_UNIXTHREAD on both legacy slices. **H08 update:** v2.4 partially closes the stdint debt — `util.c` gets `#include <stdint.h>` (D20, originally for surrogate, kept as hygiene per H04 even though surrogate now drops util.c). Still deferred to a follow-up ticket: `log.c` stdint include, conditional-compiling `compressor_page_count` for future 10.6-SDK reconsideration. | A reviewer wants log.c's stdint folded in too — trivial extension if so |
| D2 | Target x86_64 min=10.6 (no fallback to 10.7) | 10.6 is the floor of our matrix; dyld no-fallback means "fallback to 10.7" doesn't actually help any user. If 10.6 breaks for a specific deployment, fix it directly | A real 10.6-only bug surfaces; we then fix directly per §4.3 |
| D3 | Universal-only release; no thin artifact publication | Project owner's call; simplifies install story; aligns with industry norm | Multiple users hit unfixable slice-load issues → R-2 in §8 |
| D4 | `scripts/verify-legacy-slices.sh` is the load-bearing invariant | Toolchain behaviour shifts between Xcode versions; only a CI gate catches regressions reliably | Gate is too brittle and fails on every minor Xcode update |
| D5 | Tier 1 promotion only on full verify.sh evidence | Audit P15 + COMPATIBILITY.md Tier 1 definition. Single-tester noted in footnote | Reviewer wants stricter promotion criteria |
| D6 | Defer code-signing / notarization to a separate effort | Out of scope; signing tri-fat is supported; i386 + hardened-runtime is incompatible but we don't sign | We should sign now |
| D7 | Linux `-m32` surrogate: portable subset only (Option A) | Option B requires #ifdef-soup for macOS-specific code; macOS-specific paths can't run on Linux anyway | Want to invest in Option B for any reason |
| D8 | Bundle Makefile + build.yml + release.yml + script in one commit | Audit P10/P12/P19: ordering matters; partial commits would break CI | Want smaller commits for review; counter: rebase, but one commit is the safe-CI ordering |
| D9 | Add `selected_arch` to `--self-test-json` rather than a new `--info` flag | One JSON field is cheaper than a new CLI surface; verify.sh already consumes self-test | Reviewer wants a top-level diagnostic flag |
| D10 | **No self-help recovery documentation** (per Q-EXEC-1) | Project owner's call: fixed-set-of-OS-versions surface (10.4–26) is well-enumerated; the CI gate prevents structural regressions; any real failure should become a tracked bug, not a workaround. README's "If something goes wrong" section points users at GitHub issues with a list of diagnostic commands to include in the report. Cost of being wrong: if recovery cases become common, add docs in v2.4.5 | Recovery cases turn out to be more frequent than expected → revisit |
| D11 | Do not mirror the phracker SDK tarball | Audit P22 legal concern; pin checksum + cache is sufficient | Legal review clears mirroring; we revisit |
| D12 | Do not boot Snow Leopard in CI for runtime tests | Audit P23: legally/operationally risky; static gates + surrogate cover most cases without VM dependency | Reviewer has a legal path to a macOS guest image in CI |
| D13 | dyld no-fallback experiment cancelled (was Step 10 in v1) | Resolved from primary source: dyld-132.13 (10.6) and dyld-195.6 (10.7) both throw on unknown LC_REQ_DYLD and propagate to process exit — no fallback. Verified 2026-05-25 against `apple-opensource/dyld` mirror. vit9696's time better spent validating the actual fix | Source disagrees in some edge case we missed |
| D14 | Communication: do all the work, then post comprehensive issue #4 reply post-tag (per Q-EXEC-5) | No pre-announce; first contact is the polished testing ask. Reduces noise and avoids "where's the fix?" follow-ups | Reviewer prefers earlier ack of receipt |
| D15 | Linux -m32 surrogate is **blocking**, not informational (per Q-EXEC-3) | Bureaucratic gates we don't enforce are noise. Job is small, well-scoped, can't false-positive on macOS-specific changes | Surrogate produces false positives that block legitimate work — downgrade then |
| D16 | Surrogate scope is **portable subset only** (`protocol.c`, `util.c`, `cJSON.c`); excludes `selftest.c`, `log.c`, `compat.c`, `cmd-*.c` (per F04, F05) | `selftest.c` depends on `compat_*`, `run_command_capture`, `commands_count()`; `log.c` lacks `<stdint.h>` (Step 0 tech debt). Standalone `tests/surrogate_32bit_main.c` duplicates portable assertions rather than reusing `test_unit.c` (which itself includes `compat.h`) | Want to expand scope and fix `<stdint.h>` + `#ifdef`-gate compat deps as part of v2.4.4 |
| D17 | Verifier uses **symbol allowlist** (diff against checked-in baseline), not blocklist (per F11) | Blocklist of 5 symbols ignored the other 142; any newly-introduced 10.7+ symbol would slip through. Per-slice baseline files (`tests/legacy_slice_symbols_<arch>.txt`) caught Step-0-clean and committed at Step 1; any diff fails CI until a contributor justifies the change | Blocklist+INFO is enough; we don't want the friction of baseline updates on every legitimate addition |
| D18 | Verifier parses **numeric `cmd` values** including unknown `?(0x...)` format (per F10) | `otool -l` prints unknown commands as `cmd ?(0x80000028)` — v2.1's `awk /cmd LC_/` silently dropped them. A future LC_REQ_DYLD command unknown to our allowlist would have passed CI. v2.2 parses both forms and rejects any unknown bit-0x80000000 command | Cost of regex robustness on `otool` output format changes — but the format is stable across decades |
| D19 | **No self-help recovery docs**, R-0 is maintainer-only (per F16) | D10 chose issue-driven support; v2.1's §8 R-0 contradicted by saying README documents `lipo -thin`. v2.2 marks R-0 as a maintainer triage tool only, removed from any public doc | A future product owner reverses the issue-driven-support policy — at that point R-0 becomes public docs |
| D20 | `src/util.c` `<stdint.h>` include brought into v2.4.4 (per G05) | v2.2 deferred all stdint cleanup; G05 caught that this blocks the surrogate (`gcc -m32 -std=c99 -Werror` on Linux has no transitive Apple SDK includes). One-line change, no behavior risk. `log.c` stays deferred because it's not in surrogate scope | Surrogate scope expands later to include `log.c` — at that point `log.c`'s include comes with it |
| D21 | Verifier uses **closed-list `KNOWN_LCS`** with hard fail on unrecognized names (per G09) | Open-ended named-only allowlist would let future ld64 emit a new LC_REQ_DYLD command with a fresh name and bypass the gate. Closed list forces explicit review whenever Apple adds a command. Maintenance cost: a list update each major Xcode release | Maintenance cost is too high (it's been low historically — list changes ~once per macOS release); revisit |
| D22 | Verifier requires **mandatory baseline files** for legacy slices (per G08) | v2.2's "missing baseline → INFO" defeated the F11 fix's intent. v2.3 makes it a hard FAIL with clear instructions to generate. arm64 baseline stays optional (its dyld floor is modern; symbol drift isn't the same compatibility risk) | A reviewer wants arm64 baseline too — straightforward to extend |

**H08 fix:** D1 above now correctly states `util.c`'s stdint include is IN v2.4.4 scope (D20) while `log.c` and the `compressor_page_count` conditional remain deferred. Note the H04 caveat — util.c is no longer in the surrogate scope so the include is now kept as hygiene rather than as a strict requirement.

---

## 11. Out of scope (explicit)

- **No code signing or notarization.**
- **No change to arm64 build.**
- **No deprecation of any currently-supported macOS version.** 10.4 → 26 still supported.
- **No change to the QGA wire protocol.**
- **No change to in-VM `--self-test`/`--safe-test`/`--install`/`--uninstall` flag semantics.** (`--self-test-json`'s `system_info` adds the `selected_arch` field — additive, not breaking.)
- **No CI runner image pinning.** Stays on `macos-latest` / `macos-14` / `macos-15` matrix. CI gate is environment-agnostic by design.
- **No package manager integration.**
- **No retroactive evidence rewrites.**
- **No additional Mach-O slice types** (e.g., `arm64e`, `x86_64h`). Stays at i386 + x86_64 + arm64.
- **No automated mirroring of the legacy SDK.**
- **No self-help recovery documentation** (per Q-EXEC-1 / D10). README points users at issues, not at `lipo -thin` workarounds.
- **No pre-announcement to vit9696** (per Q-EXEC-5 / D14). First contact is the post-tag issue #4 reply with diagnostics.
- **No dyld no-fallback runtime experiment** (per D13). Resolved from primary source.
- **`<stdint.h>` cleanup is PARTIAL in v2.4.4 (per G05):** `src/util.c` gets the include (required to unblock the surrogate). `src/log.c` does NOT get the include in v2.4.4 (it's not in the surrogate scope; deferred to a follow-up ticket). This is the minimum scope creep needed to make Gate G2 actually buildable.
- **No expansion of surrogate scope to `selftest.c` / `log.c` / `compat.c`** — those drag macOS-specific deps. Future ticket per D16.
- **No `#ifdef` / stub layer for macOS-only code in portable files** — separate refactor. v2.4.4 scope is fix-the-bug + add-the-CI-gate, not source restructuring.
- **No automated baseline-update tooling** for `tests/legacy_slice_symbols_<arch>.txt` — manual update + PR review is the intended workflow per D17. Auto-update would defeat the gate.

---

## 12. Open questions — all resolved

All five v2 open questions were resolved during the Step-0 + Q-EXEC round on 2026-05-25. Recorded here for traceability.

1. **Q12.1. ~~Does the 10.6 SDK build succeed for our x86_64 surface?~~** → **RESOLVED (NO)**. Step 0 found three errors: `vm_statistics64.compressor_page_count` (10.9+ field used unconditionally in `cmd-hardware.c:102`), and missing `<stdint.h>` in `util.c` + `log.c`. Decision D1: stay with 10.13 SDK + explicit min flags.

2. **Q12.2. ~~i386 LC_DYLD_INFO_ONLY mitigation policy?~~** → **RESOLVED (not needed)**. Step 0 confirmed 10.13 SDK + `-mmacosx-version-min=10.4` for i386 emits classic relocations only — no `LC_DYLD_INFO_ONLY`. R13 closed.

3. **Q12.3. ~~Recovery script as 6th asset?~~** → **RESOLVED (NO)**, per Q-EXEC-1 / D10. Issue-driven support. README's "If something goes wrong" section asks users to file issues with diagnostic-command outputs.

4. **Q12.4. ~~Step 10 dyld no-fallback experiment?~~** → **RESOLVED from primary source** (D13). dyld-132.13 (10.6) and dyld-195.6 (10.7) both confirmed via `apple-opensource/dyld` mirror — no fallback, throws on unknown LC_REQ_DYLD. vit9696's time not needed for this.

5. **Q12.5. ~~Surrogate-32bit blocking or informational?~~** → **RESOLVED (BLOCKING)**, per Q-EXEC-3 / D15.

---

## 13. References

### Apple open-source citations

URLs point to current `main` branches of `apple-oss-distributions`. For stable citation at sign-off, replace `/blob/main/` with `/blob/<commit-sha>/`.

| Source | Path | Search for |
|---|---|---|
| `apple-oss-distributions/ld64` | `src/ld/Options.cpp` | `fEntryPointLoadCommand`, `-no_new_main` |
| `apple-oss-distributions/ld64` | `src/ld/ld.hpp` | `version2012` |
| `apple-oss-distributions/ld64` | `src/ld/PlatformSupport.cpp` | macOS min OS table |
| `apple-oss-distributions/dyld` | `common/MachOFile.cpp` | `GradedArchitectures::grade` |
| `apple-oss-distributions/dyld` | `common/MachOAnalyzer.cpp` | `fatButMissingSlice` |
| `apple-oss-distributions/dyld` | `mach_o/UnsafeHeader.cpp` | `"unknown required load command"` |
| `apple-oss-distributions/xnu` | `EXTERNAL_HEADERS/mach-o/loader.h` | `LC_REQ_DYLD`, `LC_MAIN`, `entry_point_command` |
| `apple-opensource/dyld` | `132.13/src/dyld.cpp` (line 1697 etc.) | `fatFindBest()` — Snow Leopard 10.6 single-slice-pick logic |
| `apple-opensource/dyld` | `132.13/src/ImageLoaderMachO.cpp` (line 340-341) | `"unknown required load command 0x%08X"` throw on LC_REQ_DYLD — Snow Leopard 10.6 |
| `apple-opensource/dyld` | `195.6/src/dyld.cpp` (line 1885 etc.) | `fatFindBest()` — Lion 10.7 (identical) |
| `apple-opensource/dyld` | `195.6/src/ImageLoaderMachO.cpp` (line 358-359) | Identical throw — Lion 10.7 |

### Authoritative blog / bug reports

- Mike Ash, "Friday Q&A 2012-11-30: Let's Build A Mach-O Executable" (LC_MAIN context)
- Mozilla bug 602049 — `dyld: unknown required load command 0x80000022 Trace/BPT trap` on 10.5
- Howard Oakley (eclecticlight.co) — "Universal Binaries: inside Fat Headers"
- Patrick Wardle (objective-see.org) — "Apple Gets an 'F' for Slicing Apples"
- Apple Developer Forums #693228 — x86_64 daemon won't launch (EBADARCH)

### Project files

- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/Makefile` (build recipes)
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/.github/workflows/build.yml`
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/.github/workflows/release.yml`
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/src/service.c` (lines 149-150)
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/src/cmd-hardware.c` (lines 14-22, 79 — weak_import pattern)
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/src/selftest.c` (`selected_arch` insertion point)
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/scripts/build-pkg.sh`
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/scripts/verify-installer.sh`
- `/Users/mav2287/Repositories/mav2287/mac-guest-agent/configs/com.macos.guest-agent.plist` (arch-neutral; no change)

### Verification artifacts (local, ephemeral; reproducible via §3.1 transcript)

- `/tmp/lc-test/b-explicit106` — proof binary from §3.1, x86_64 + min=10.6, contains LC_UNIXTHREAD (reproduce: see §3.1 command transcript)
- `/tmp/lc-test/c-explicit107` — boundary test, min=10.7, LC_UNIXTHREAD
- `/tmp/lc-test/d-explicit108` — boundary test, min=10.8, LC_MAIN
- `/tmp/mga-bins/mga-trifat` — packaging proof from §3.4 (uses v2.4.3 inputs; for new pipeline outputs use §5 Step 1.4)

### Audit documents addressed

- `audit.md` (project-level): A1 (single-universal-binary not yet implemented) → addressed in §4.1, §5 Steps 2/3/4/5/6. A2 (legacy x86_64 not enforced as CI invariant) → addressed in §4.5 G1, §6.2 `scripts/verify-legacy-slices.sh`.
- Plan v1 inline `<AUDIT FINDING>` tags (20 items): P1–P23 → each resolved or explicitly deferred in the relevant §3/§5/§6/§9/§10 section of this v2.

---

## 14. Sign-off blocks (to be filled at audit)

### James (project owner)
- [ ] §1 executive summary matches my mental model
- [ ] §4.1 chosen approach is what I want
- [ ] §4.4 universal-only mitigations are sufficient
- [ ] §4.6 vit9696-unavailable contingency + Q-EXEC-5 communication path are acceptable
- [ ] §6 diffs would land cleanly
- [ ] §7 success criteria are sufficient
- [ ] §8 rollback plan is acceptable
- [ ] §9 risk register has no gaps I can identify
- [ ] §10 decision log: D1, D10, D13, D14, D15 (the Q-EXEC + Step-0 decisions) are correctly reflected
- [ ] §12 — all 5 open questions resolved as recorded (no overrides)

### Neutral third-party reviewer
**WAIVED** per project owner's call on 2026-05-25. The audit-review phase that produced the inline `<AUDIT FINDING>` tags (P-series in v1, F-series in v2, G-series in v2.2 → v2.3, H-series in v2.3 → v2.4) is complete; all findings addressed in this v2.4 revision. Project owner signs alone for v2.4.4 execution.

**Plan v2.4 is approved for execution when James's block is signed.**
