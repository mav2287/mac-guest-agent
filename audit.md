# Project Audit

Original audit date: 2026-05-23
Updated: 2026-05-26
Repository: `/Users/mav2287/Repositories/mav2287/mac-guest-agent`
Branch audited: `universal-upgrade-v2.4.4`
HEAD audited: `d47ebef 10.11.6 evidence: v2.5.0 verifier run + plug VMID redaction leak`

Scope: validation of the previous audit items plus a fresh audit of the current
tree, with extra attention on the new universal binary.

## Summary

The previous `audit.md` findings are addressed in the current tree:

- The release-toolchain risk is now explicitly documented in the workflows,
  including `ld_classic` as the load-bearing dependency and a fallback chain.
- `scripts/install.sh --local` now recognizes
  `mac-guest-agent-darwin-universal`, `/tmp/mac-guest-agent-darwin-universal`,
  `build/mac-guest-agent-universal`, and an explicit `--local PATH`.
- README now documents the legacy 10.13 SDK requirement and
  `LEGACY_SDK=/tmp/MacOSX10.13.sdk`.
- Compatibility docs now describe universal slices instead of separate/thin
  binaries.
- The stale `host_statistics64` source comment is corrected.
- `universal_upgrade.md` is marked historical/superseded and lists the known
  divergences from the final implementation.

The rebuilt universal binary itself passes the important structural checks:

- Fat header contains exactly `i386 x86_64 arm64`.
- i386: `LC_VERSION_MIN_MACOSX 10.4`, `LC_UNIXTHREAD`, allowed dylibs, symbol
  baseline match, `_host_statistics64` weak-import preserved.
- x86_64: `LC_VERSION_MIN_MACOSX 10.6`, `LC_UNIXTHREAD`, allowed dylibs,
  symbol baseline match, `_host_statistics64` weak-import preserved.
- arm64: `LC_BUILD_VERSION minos 11.0`, `LC_MAIN`, allowed dylibs,
  `_host_statistics64` weak-import preserved.
- The universal artifact runs on the current arm64 host, and the x86_64 slice
  runs under Rosetta with `selected_arch=x86_64`.

No blocker was found in the current universal binary's local Mach-O structure.
The highest-risk remaining issue is evidence quality: the project still lacks a
repeatable target-OS runtime harness for the actual current universal artifact
on the oldest and transition OSes. The other findings are release-gate,
documentation, workflow-lint, testability, or packaging issues that can mislead
users or future release audits, but they are not confirmed slice-load failures.

## Findings

### High: current universal runtime support is not backed by a repeatable target-OS VM harness

Current state:

- Local inventory found QEMU 10.2.2 installed, but no usable local macOS VM
  images or macOS installer media in the checked common locations:
  `~/Virtual Machines.localized`, `~/Documents`, `~/Downloads`, and the UTM
  container path.
- `utmctl` and `virsh` are not installed. `prlctl list -a` shows only an
  invalid Windows ARM VM, not a usable macOS target.
- Existing evidence covers:
  - 10.4.11 Tiger with older v2.4.2 evidence, not the current v2.5.0 universal
    i386 slice.
  - 10.11.6 El Capitan with current v2.5.0 universal x86_64 evidence.
- There is no current v2.5.0 runtime evidence for the actual universal i386
  slice on Tiger/Leopard, no Snow Leopard/Lion x86_64 transition evidence, and
  no Big Sur arm64 floor evidence.
- `tests/test_legacy_slice_gate.sh` is useful and passes four
  verifier-sabotage cases locally, but it is still a static verifier test, not
  a target-OS boot/run harness.

Why this matters:

Static gates are strong enough to catch the known LC_MAIN/load-command failure
class, and the current x86_64 universal slice has real 10.11 evidence. They do
not prove that dyld and libSystem on Tiger/Leopard/Snow Leopard/Lion/Big Sur can
execute the current universal slice end to end. Because the project now ships a
single universal artifact with no published thin fallback, runtime validation
should be reproducible without depending on a volunteer with old Intel Macs.

Recommended fix:

- Add a maintained target-OS validation harness that can run against
  user-supplied VM disks/installers. Minimum profiles:
  - Tiger/Leopard i386.
  - Snow Leopard or Lion x86_64.
  - Big Sur arm64.
- Keep it explicit that CI can run this only where the required guest assets are
  available; otherwise make it a documented self-hosted/nightly/manual harness,
  not an implied GitHub-hosted check.
- The harness should inject the release artifact, run loader-safe checks
  (`file`, `lipo -info`), then run `--version`, `--self-test-json`, and the
  host-side `scripts/verify.sh` path where QGA transport is available.
- Store successful runs under `docs/evidence/<version>/` with
  `system_info.selected_arch` captured.

### Medium: arm64 Big Sur compatibility has no symbol-baseline gate

Current state:

- `scripts/verify-legacy-slices.sh:241-243` checks arm64 load commands and
  `minos=11.0`.
- `scripts/verify-legacy-slices.sh:198-218` treats missing baselines as a hard
  failure for i386/x86_64, but only informational for arm64.
- There is no `tests/legacy_slice_symbols_arm64.txt`.
- `docs/COMPATIBILITY.md:128-132` documents arm64 undefined symbols as
  "varies (modern)".

Why this matters:

The current arm64 slice runs on macOS 26.5 and advertises `minos 11.0`, but the
CI gate would not catch a future direct import of a macOS 12+ symbol. That would
preserve the load-command floor while still breaking Big Sur-era arm64 hosts.
This is the arm64 equivalent of the legacy symbol-drift risk already handled
for i386 and x86_64.

Recommended fix:

- Add an arm64 undefined-symbol baseline and make it required, or add a
  separate arm64 symbol allowlist keyed to the 11.0 support floor.
- Keep the existing arm64 load-command/minos checks; this is an additional
  API-availability guard, not a replacement.

### Medium: installer symbol verification is stale and not derived from the actual slice imports

Current state:

- `scripts/verify-installer.sh:384-391` and
  `scripts/verify-all-installers.sh:170-173` use a hand-maintained list of 19
  "critical" symbols.
- That list still checks `_poll` and `_host_statistics`, but the current i386
  slice imports neither. The current code deliberately uses `select()`, not
  `poll()`, for the Tiger serial path.
- The current i386 undefined-symbol baseline has 147 symbols. Many current
  imports are not in the installer symbol check at all, including `_select`,
  `_getopt_long`, `_atoll`, `_strtoll`, `_setenv`, `_inet_ntop`, `_lchown`, and
  the CoreFoundation/IOKit entry points.
- `docs/COMPATIBILITY.md:96-97` repeats the stale "19 required symbols" model.

Why this matters:

The per-slice baselines prove the build's import set has not drifted from the
checked-in baseline, but they do not prove those imports exist on each target
OS. The installer verifier is supposed to be the target-library side of that
proof. Because it is stale and partial, an installer could pass "Deep verify"
while still missing a real imported symbol from the current slice, or fail on a
symbol the current binary no longer uses.

Recommended fix:

- Generate target-OS symbol checks from `tests/legacy_slice_symbols_i386.txt`
  and `tests/legacy_slice_symbols_x86_64.txt`, with explicit allow/ignore
  rules for weak imports and framework symbols.
- Remove `_poll` from the hard-coded check or, preferably, remove the
  hard-coded list entirely.
- Update `docs/COMPATIBILITY.md` to describe the baseline-derived check rather
  than the obsolete 19-symbol list.

### Medium: UTM, manpage, and CLI transport guidance still contradict the ISA-serial contract

Current state:

- `README.md:15`, `README.md:44-46`, `docs/PLATFORMS.md:12-17`, and
  `src/channel.c:31-57` establish the current contract: ISA serial is the
  primary/default transport across host classes; VirtIO is only a fallback for
  selected non-ISA configurations.
- `docs/UTM.md:17-23` still tells users to add a VirtIO serial device, and
  `docs/UTM.md:40-43` says a successful self-test should report
  `/dev/cu.virtio`.
- `docs/UTM.md:212-215` says Virtualization.framework macOS guests use
  `/dev/cu.virtio`, which conflicts with the README's warning that Apple's
  own agent claims that channel on VZ-backed hosts.
- `docs/mac-guest-agent.8.in:97-98` and generated
  `docs/mac-guest-agent.8:97-98` say Big Sur+ default virtio-serial "also
  works via Apple's built-in VirtIO driver."
- `docs/PVE.md:370` says `type=isa` is required only for pre-Big Sur, while
  `docs/PVE.md:313` correctly says `type=isa` is required even on Big Sur+.
- `src/main.c:35-37`, `src/main.c:144-150`, and `src/main.c:173-176` still
  present `method = virtio-serial` as the default; locally,
  `./build/mac-guest-agent-universal --dump-conf` prints
  `method = virtio-serial`, even though the channel auto-detection order starts
  with ISA serial devices.

Why this matters:

A user following the UTM guide, manpage, or `--dump-conf` output can configure
only a VirtIO channel while the current project guidance says ISA is the
supported universal transport. On Big Sur+ VZ-backed guests, that can put the
host on Apple's 18-command agent or create a channel conflict instead of using
this agent with freeze support. This is not a Mach-O slice problem, but it is a
real operational compatibility risk.

Recommended fix:

- Rework `docs/UTM.md` to match the ISA-first policy, or explicitly split UTM
  QEMU-backend guidance from VZ-backed macOS guidance and state what is
  supported.
- Fix the manpage template and regenerate `docs/mac-guest-agent.8`.
- Change CLI/default config wording from `virtio-serial` to an accurate
  `auto`/ISA-first description, or wire `method` into real behavior if it is
  meant to be user-facing.
- Keep VirtIO fallback documentation narrowly scoped as an advanced fallback,
  not the default path.

### Low: universal artifact test output reports the wrong selected architecture

Current state:

- `tests/run_tests.sh:26-29` detects the binary architecture by running
  `file "$BINARY" | grep -o 'arm64\|x86_64\|i386' | head -1`.
- For `build/mac-guest-agent-universal`, `file` lists the fat header in
  `i386 x86_64 arm64` order, so the test header prints:
  `Binary: ./build/mac-guest-agent-universal (i386)`.
- The process actually runs the arm64 slice on this host; `--self-test-json`
  reports `system_info.selected_arch=arm64`.

Why this matters:

The release workflow runs the integration suite against
`build/mac-guest-agent-universal`. The tests pass, but the evidence header is
misleading precisely for the universal artifact. This can confuse future audit
records and make it look like modern macOS executed the i386 slice.

Recommended fix:

- For fat binaries, print both the fat slice list and the executed slice.
- The simplest reliable source for the executed slice is
  `"$BINARY" --self-test-json | system_info.selected_arch`, with `file`/`lipo`
  used only for the available slice list.

### Low: package helper advertises double-click install, but the generated pkg is unsigned

Current state:

- `Makefile:151-153` and `scripts/build-pkg.sh:8-10` describe the `.pkg` as
  double-click installable.
- `make pkg LEGACY_SDK=/tmp/MacOSX10.13.sdk` builds
  `build/mac-guest-agent-2.5.0-universal.pkg`.
- `pkgutil --check-signature` reports `Status: no signature`.
- `spctl --assess --type install` rejects the package with
  `source=no usable signature`.
- Expanding the pkg confirms the payload binary is tri-fat and reports 2.5.0,
  but `lsbom` shows 11 zero-byte `._*` AppleDouble/provenance entries in the
  package BOM.
- The release workflow does not publish the pkg, so this does not affect the
  current universal binary release asset.

Why this matters:

Terminal installation with `sudo installer -pkg ... -target /` remains usable,
but the "double-click" wording is optimistic on modern macOS Gatekeeper
defaults. If the pkg helper is kept, it should either produce a signed package
when credentials are available or document the terminal-install path as the
supported path for unsigned local packages.

Recommended fix:

- Reword the pkg helper output/comments to avoid promising Finder install for
  unsigned packages, or add a signed `productsign` path when a Developer ID
  Installer identity is available.
- Strip avoidable AppleDouble/provenance entries from the pkg staging tree
  before `pkgbuild` if the pkg helper remains supported.

### Low: install and update paths lack a safe dry-run or staging mode

Current state:

- `scripts/install.sh --help` works without root, but
  `scripts/install.sh --local /tmp/does-not-exist` exits at the root check
  before validating the supplied path.
- `mac-guest-agent --update /tmp/does-not-exist` exits at the root check before
  validating the supplied update file.
- The script and binary hardcode live system paths such as `/usr/local/bin` and
  `/Library/LaunchDaemons`, with no `DESTDIR`, `PREFIX`, `DRY_RUN`, or staging
  root override.

Why this matters:

The user-facing install and update paths cannot be exercised end to end in CI
or in this audit without touching live system locations as root. That leaves
the most operationally important paths covered mostly by static review and
partial smoke checks rather than repeatable tests.

Recommended fix:

- Add a dry-run/staging mode to `scripts/install.sh` and the service
  install/update code, or factor the filesystem operations behind overridable
  paths for test execution.
- Validate explicit `--local PATH` and `--update PATH` inputs before the root
  check where possible, so argument/path behavior can be tested without root.

### Low: generated/reference docs still report 44 commands even though the binary registers 45

Current state:

- `./build/mac-guest-agent-universal --test` reports 45 supported commands
  through `guest-info`.
- `./scripts/gen-command-table.sh ./build/mac-guest-agent-universal` passes:
  `Commands registered: 45` and `Commands in docs/COMMAND_STATUS.md: 45`.
- `docs/mac-guest-agent.8.in:100-102` and generated
  `docs/mac-guest-agent.8:100-102` still say 44 commands and list stale
  category counts.
- `docs/ARCHITECTURE.md:68` still says the registry has 44 commands.

Why this matters:

The source of truth and `docs/COMMAND_STATUS.md` agree on 45 commands, so this
is documentation drift, not a runtime defect. The stale text is in generated
user-facing manpage output, though, so it will keep reappearing until the
template is corrected and regenerated.

Recommended fix:

- Update `docs/mac-guest-agent.8.in`, regenerate `docs/mac-guest-agent.8`, and
  update `docs/ARCHITECTURE.md`.
- Also update `docs/COMMAND_STATUS.md:54`: it still says
  `guest-fsfreeze-freeze-list` does not filter mountpoints, while
  `src/cmd-fs.c:674-735` and `docs/BACKUP.md:37` describe the implemented
  mountpoint-filtering behavior.
- Consider extending the existing docs/binary command check so it also catches
  command-count claims in the manpage template and freeze-list behavior claims
  in `docs/COMMAND_STATUS.md`.

### Low: local workflows do not pass full actionlint because embedded shell snippets have ShellCheck warnings

Current state:

- Installed `actionlint` 1.7.12 via Homebrew and ran it against the local
  workflow files.
- `actionlint -shellcheck ''` passes, and `actionlint -ignore 'shellcheck
  reported issue'` passes, so the workflow YAML/schema layer is valid.
- Full `actionlint` exits non-zero because ShellCheck flags embedded `run:`
  snippets:
  - `.github/workflows/build.yml:65`: `ls src/*.c | grep -v third_party`,
    unquoted command substitution, and unquoted `$f`.
  - `.github/workflows/build.yml:104`: `ls ... | grep -i xcode` diagnostics.
  - `.github/workflows/release.yml:29`: unquoted `$GITHUB_ENV`.

Why this matters:

These are not current CI blockers by themselves, but they prevent using
`actionlint` as a clean workflow gate. If workflow linting is added later, it
will fail on existing warnings before catching more meaningful release-flow
errors.

Recommended fix:

- Clean up the shell snippets or add narrow actionlint ignore comments for
  intentionally diagnostic `ls | grep` lines.
- Prefer `find`/globs for the static analyzer source list and quote
  substitutions (`"$(xcrun --show-sdk-path)"`, `"$f"`, `"$GITHUB_ENV"`).

## Validation Performed

Commands and inspections used for this refresh:

- Reviewed old `audit.md` against the current tree.
- Reviewed current `Makefile`, `.github/workflows/build.yml`,
  `.github/workflows/release.yml`, `scripts/install.sh`,
  `scripts/build-pkg.sh`, `scripts/verify-legacy-slices.sh`,
  `scripts/verify.sh`, `tests/run_tests.sh`, `src/selftest.c`,
  `src/cmd-hardware.c`, README, CHANGELOG, compatibility docs, and
  `universal_upgrade.md`.
- Ran `bash -n` on shell scripts.
- Ran ShellCheck across shell scripts; `shellcheck -S error` passed. Remaining
  ShellCheck output is style/intentional dynamic-shell noise, not audit
  findings.
- Ran `make clean`.
- Ran `make build-all LEGACY_SDK=/tmp/MacOSX10.13.sdk` from a clean tree.
- Ran `./scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests`.
- Sabotaged verifier inputs to confirm failure paths:
  x86_64 `LC_MAIN`/wrong-min slice, missing legacy baseline, undefined-symbol
  baseline drift, and strong `_host_statistics64`.
- Inspected `lipo -info`, `lipo -detailed_info`, `file`, `otool -l`,
  `otool -L`, `nm -m`, and undefined-symbol counts for all slices.
- Compared current arm64 undefined symbols against the macOS SDK stubs/headers
  for the new arm64-only imports observed in this build.
- Ran `./build/mac-guest-agent-universal --version`.
- Ran `./build/mac-guest-agent-universal --self-test-json`.
- Ran `arch -x86_64 ./build/mac-guest-agent-universal --version`.
- Ran `arch -x86_64 ./build/mac-guest-agent-universal --self-test-json`.
- Ran `./tests/run_tests.sh ./build/mac-guest-agent-universal`.
- Ran `./tests/run_tests.sh ./build/mac-guest-agent-x86_64`.
- Ran `make test`.
- Ran `make test-coverage`.
- Ran `clang --analyze` over the source files.
- Ran `make dist LEGACY_SDK=/tmp/MacOSX10.13.sdk`.
- Ran `make pkg LEGACY_SDK=/tmp/MacOSX10.13.sdk`.
- Ran `ruby` YAML parsing against `.github/workflows/build.yml` and
  `.github/workflows/release.yml`.
- Installed and ran `actionlint` 1.7.12.
- Ran `gh workflow view --ref universal-upgrade-v2.4.4 --yaml` for both
  workflows to confirm the remote branch-ref workflow content matches the local
  universal-only flow. The default-branch workflow view is older and was not
  treated as the audited state for this branch.
- Ran `plutil -lint configs/com.macos.guest-agent.plist`.
- Ran `./scripts/gen-command-table.sh ./build/mac-guest-agent-universal`.
- Checked `codesign`, `spctl`, and `pkgutil` assessment behavior.
- Expanded `build/mac-guest-agent-2.5.0-universal.pkg` with
  `pkgutil --expand-full`, inspected `PackageInfo`, `Bom`, preinstall,
  postinstall, and payload files.
- Ran `pkgutil --payload-files` and `lsbom` against the generated pkg.
- Ran installer/update smoke checks that are safe without root:
  `scripts/install.sh --help`, `scripts/install.sh --local /tmp/does-not-exist`
  (root check fires first), `--update` without an argument, and `--update` with
  a missing path.
- Inventoried local VM tooling and guest assets: QEMU present; no usable UTM,
  libvirt, Parallels macOS VM, or macOS install media found in common local
  locations.
- Ran `tests/test_legacy_slice_gate.sh`, the tracked verifier-sabotage script.

Current local results:

- Universal artifact: `i386 x86_64 arm64`, 440K.
- `scripts/verify-legacy-slices.sh`: passed for i386, x86_64, and arm64.
- Universal arm64 execution: `2.5.0`, `selected_arch=arm64`, `errors=0`,
  `status=pass`.
- Universal x86_64 execution under Rosetta: `2.5.0`,
  `selected_arch=x86_64`, `errors=0`, `status=pass`.
- Universal integration suite: 77 passed, 0 failed, 5 skipped.
- x86_64 thin-slice integration suite under Rosetta: 77 passed, 0 failed,
  5 skipped.
- Full `make test`: unit 48/0, proactive 128/0, fuzz 210k rounds passed,
  integration 77/0/5 skipped, verify-transport shims 57/0.
- `make test-coverage`: passed and produced an llvm-cov report.
- `clang --analyze`: no analyzer findings.
- `shellcheck -S error`: passed.
- Workflow YAML parsed successfully with Ruby.
- `actionlint -shellcheck ''`: passed.
- `actionlint -ignore 'shellcheck reported issue'`: passed.
- Full `actionlint`: failed on ShellCheck warnings in workflow `run:` blocks.
- LaunchDaemon plist passed `plutil -lint`.
- `make dist`: produced only `dist/mac-guest-agent-darwin-universal` and
  `dist/checksums.sha256`; checksum file covers the universal artifact.
- `make pkg`: produced `build/mac-guest-agent-2.5.0-universal.pkg`.
- Expanded pkg payload contains the expected tri-fat binary, manpage, default
  config, preinstall, and postinstall scripts. The payload binary reports
  `mac-guest-agent 2.5.0` and contains `i386 x86_64 arm64`.
- `lsbom` shows 11 zero-byte `._*` AppleDouble/provenance entries in the pkg
  BOM. This is messy but secondary to the unsigned-pkg finding because the pkg
  is not currently published.
- `scripts/gen-command-table.sh`: binary and `docs/COMMAND_STATUS.md` both
  report 45 commands.
- `tests/test_legacy_slice_gate.sh`: 4 passed, 0 failed
  (`LC_MAIN`, wrong x86_64 min version, missing i386 slice, and symbol-baseline
  drift were all rejected).

Validation limitations:

- I still cannot execute the i386 slice on this modern macOS host.
- No local bootable target macOS VM assets were found for Tiger, Leopard, Snow
  Leopard, Lion, or Big Sur, so no new target-OS runtime evidence was produced.
- I did not execute the Linux `surrogate-32bit` CI job locally; the workflow
  job is still the intended environment for that check.
- I did not safely execute the install/update paths as root against `/usr/local`
  or `/Library/LaunchDaemons`; the current scripts do not provide a dry-run or
  staging root for that.
- I did not boot Big Sur, Tiger, Leopard, Snow Leopard, or Lion VMs locally.

## Worktree Notes

Pre-existing untracked content is present under `docs/drafts/`.

`audit.md` is intentionally left untracked and should not be added or committed
unless explicitly requested.
