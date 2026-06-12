# Repository Audit - 2026-05-29

Audited commit: `36a7425` (`main`, `origin/main`)

Scope: tracked source, scripts, workflows, docs, configs, tests, generated release artifacts, and generated package output rebuilt from this tree. This pass rechecked the previously reported issues and then audited the current project state from scratch.

Untracked local files observed but not treated as project source: `.claude/settings.json`, `compile_flags.txt`.

## Bottom Line

I did not find a direct blocker in the current universal binary. The rebuilt artifact is a tri-fat Mach-O (`i386 x86_64 arm64`), the legacy slices still use `LC_UNIXTHREAD`, deployment floors match the support contract (`i386=10.4`, `x86_64=10.6`, `arm64=11.0`), undefined-symbol baselines match, and native arm64 plus Rosetta x86_64 runtime checks passed on macOS 26.5.

The previous nine audit findings are addressed. The remaining findings are in adjacent release/install/verification paths: the older binary-level `--update` path is now weaker than the new installer upgrade path, the generated `.pkg` can report success even if service installation fails, and the libvirt verifier is still too brittle for valid XML attribute ordering.

## Current Findings

### MED-1 - Binary `--update` is now a stale and unsafe upgrade path

`src/service.c` still advertises and implements `mac-guest-agent --update PATH`, and the CLI docs still list it (`docs/CLI.md:20-28`, `src/main.c:203-214`). That path stops/unloads the service, renames the current binary to `/usr/local/bin/mac-guest-agent.backup`, copies the replacement, verifies only `-V`, then runs `launchctl load/start` (`src/service.c:259-319`).

Problems:

- On copy failure or `-V` failure, it renames the backup back into place but does not reload/reinstall the restored service before returning non-zero (`src/service.c:293-310`). The old binary can be back on disk while the LaunchDaemon remains unloaded.
- It does not regenerate the LaunchDaemon plist from the replacement binary, while the new `scripts/install.sh --upgrade` path explicitly does. If the embedded plist changes between releases, `--update` leaves stale service metadata behind.
- It does not perform the mode-aware functional checks now present in `scripts/install.sh --upgrade`.

Impact: users following the documented binary-level update path can get a different and less safe result than users following the new installer state machine. For a broken or wrong-arch replacement binary, rollback can leave the agent stopped.

Fix: either deprecate/remove `--update` from help/docs and direct users to `scripts/install.sh --local PATH --upgrade`, or rework `service_update()` to match the new upgrade contract: restore and re-run `--install` on rollback, regenerate the plist on success, verify service state, and remove the backup only after success.

### MED-2 - Generated `.pkg` postinstall masks service-install failures

`scripts/build-pkg.sh` generates a `postinstall` script that runs:

```bash
/usr/local/bin/mac-guest-agent --install 2>/dev/null || true
echo "macOS Guest Agent installed."
exit 0
```

Source: `scripts/build-pkg.sh:92-103`.

Impact: package installation can report success even if the universal binary cannot execute on that machine, `--install` fails, the plist cannot be written, or `launchctl load` fails. This matters for the universal-binary support boundary: on an unsupported architecture, or with a bad package payload, the package can leave files behind while telling the operator the agent installed.

Fix: remove `2>/dev/null || true`, let `--install` output its actual error, and exit non-zero on failure. Consider an explicit `uname -m` gate in the generated script so unsupported PowerPC Tiger/Leopard fails with the same clear message as `scripts/install.sh`.

### MED-3 - libvirt verifier rejects valid ISA serial XML when attributes are reordered

The original libvirt bug is fixed in direction: the verifier now looks for ISA serial instead of the old VirtIO channel. The current check is still too narrow:

```bash
grep -qE "target type=['\"]isa-serial['\"]"
```

Source: `scripts/verify.sh:679-704`.

That only accepts XML where `type` is the first attribute on the `<target>` element. Valid libvirt XML such as this is rejected:

```xml
<target port='0' type='isa-serial'/>
```

I reproduced the false negative with a temporary `virsh` shim: `scripts/verify.sh --transport libvirt ...` reported `FAIL guest-agent ISA serial target missing` even though the domain XML contained an ISA serial target. The new tests only cover the type-first form (`tests/test_verify_transports.sh:506-560`), so this edge is not locked.

Impact: valid libvirt configurations can be reported as misconfigured. This does not break the agent runtime, but it undermines the host-side verifier and can send users chasing a non-existent transport problem.

Fix: parse the XML structurally, or at minimum match a `<target` tag containing `type='isa-serial'` or `type="isa-serial"` in any attribute position, for example `grep -Eq "<target[^>]*type=['\"]isa-serial['\"]"`. Add a regression fixture with attributes reordered.

### LOW-1 - `--virtio` operator-config refusal suggests an incomplete manual path

When `/etc/qemu/qemu-ga.conf` already exists, `scripts/install.sh --virtio` now refuses before clobbering it. That fixes the data-loss issue. The remediation text then says to hand-edit the file to add the VirtIO path and "run the standard install (no --virtio flag)" (`scripts/install.sh:732-738`).

For the unsupported Big Sur+/kubevirt-style VirtIO override, the standard install does not unload Apple's `AppleQEMUGuestAgent`, does not verify the VirtIO device was released, and does not drop the marker that lets `install.sh --uninstall` restore Apple state. Those are the core steps documented for the gated override path (`docs/NO_ISA_OVERRIDE.md:67-75`).

Impact: an operator following the second suggested remediation can end up with a config pointing at VirtIO while the Apple daemon still owns the channel, and future uninstall will not know to restore Apple state.

Fix: change the message to prefer "back up the config and retry `--virtio`" for the managed path. If the manual path remains documented in the error text, explicitly say the operator must manually unload/restore AppleQEMUGuestAgent and will not get marker-managed rollback.

### LOW-2 - Some release/test-count docs are stale after the latest test expansion

Examples:

- `CHANGELOG.md:58` still says `tests/test_install_flags.sh` is a 34-assertion suite. The current run reports 79 install-flag assertions.
- `docs/COMPATIBILITY.md:261-266` still lists older quality metrics and says the test suite is `48 unit + 31 proactive + 210k fuzz + 63 integration`. The current local run reports 48 unit, 128 proactive, 210k fuzz, 78 integration, 63 verify-transports, and 79 install-flags.

Impact: low runtime risk, but it misstates the validation surface in the docs users rely on when judging the universal-binary release quality.

Fix: update the counts or avoid exact counts in long-lived docs unless they are generated.

## Previous Finding Re-check

All nine previously recorded findings were rechecked against `36a7425`:

- Previous HIGH-1, config clobber in `--virtio`: resolved. `detect_install_state()` and `operator_config_exists()` now refuse fresh `--virtio` / `--virtio-force` when an existing install or operator config is present (`scripts/install.sh:181-229`, `scripts/install.sh:721-739`). Test coverage exists in `tests/test_install_flags.sh`.
- Previous MED-1, failed `--virtio` rollback leaving binary/plist: resolved for the fresh `--virtio` functional-verify failure path. It now calls the binary's `--uninstall` when possible, removes the binary, removes config/marker, and reloads Apple best-effort (`scripts/install.sh:454-473`).
- Previous MED-2, unknown installer args ignored: resolved. Unknown flags and extra args after `--local PATH` now fail (`scripts/install.sh:773-809`) and tests cover the cases (`tests/test_install_flags.sh:141-155`).
- Previous MED-3, libvirt verifier required VirtIO instead of ISA: direction fixed. See current MED-3 for the remaining attribute-order robustness bug.
- Previous MED-4, CI omitted install-flag tests: resolved. Build and release workflows both run `make test-install-flags`; release also runs `make test-verify-transports` (`.github/workflows/build.yml:193-197`, `.github/workflows/release.yml:52-56`).
- Previous MED-5, no libvirt transport shim tests: resolved at the basic level. `tests/test_verify_transports.sh` now includes a `virsh` shim pass/fail block (`tests/test_verify_transports.sh:448-561`).
- Previous LOW-1, `NO_ISA_OVERRIDE.md` overstated functional verification: resolved. It now states the check is PID plus fresh log line, not a host-to-guest QGA round trip (`docs/NO_ISA_OVERRIDE.md:67-75`).
- Previous LOW-2, testing harness hardcoded stale latest version: resolved with version-agnostic wording in `docs/TESTING_HARNESS.md`.
- Previous LOW-3, PVE config comments pushed thin per-arch builds: resolved. The PVE examples now point at the universal artifact.

## Universal Binary and Package Validation

- `make clean && make build-all LEGACY_SDK=/tmp/MacOSX10.13.sdk` passed.
- `build/mac-guest-agent-universal` is tri-fat: `i386 x86_64 arm64`.
- `./scripts/verify-legacy-slices.sh build/mac-guest-agent-universal tests` passed:
  - i386: min macOS 10.4, `LC_UNIXTHREAD`, dependency allowlist clean, symbols match, weak imports preserved.
  - x86_64: min macOS 10.6, `LC_UNIXTHREAD`, dependency allowlist clean, symbols match, weak imports preserved.
  - arm64: min macOS 11.0, `LC_MAIN`, dependency allowlist clean, symbols match.
- Universal checksum from this build: `dfd734a6e384fe05c6b8056a58acb1cd1320cc55325ef02f8521ee588024f77b`.
- Native arm64 and Rosetta x86_64 `--version` both returned `mac-guest-agent 2.5.3`.
- Native arm64 and Rosetta x86_64 `--self-test-json` both returned `status=pass`, `errors=0`, with `selected_arch` matching the launched slice.
- `LEGACY_SDK=/tmp/MacOSX10.13.sdk bash tests/test_legacy_slice_gate.sh` passed all sabotage checks.
- `make dist LEGACY_SDK=/tmp/MacOSX10.13.sdk` passed, and `dist/mac-guest-agent` passed the legacy-slice verifier.
- `make pkg LEGACY_SDK=/tmp/MacOSX10.13.sdk` passed. The expanded payload binary passed the legacy-slice verifier.
- The `.pkg` is unsigned and `spctl --assess --type install` rejects it with `source=no usable signature`; this is expected and documented.

## Validation Performed

- `make test LEGACY_SDK=/tmp/MacOSX10.13.sdk` passed.
- Unit tests: 48 passed, 0 failed.
- Proactive tests: 128 passed, 0 failed.
- Fuzz tests: 210k rounds passed under ASAN/UBSAN.
- Integration tests: 78 passed, 0 failed, 5 skipped.
- Verify-transports tests: 63 passed, 0 failed.
- Install-flag tests: 79 passed, 0 failed.
- `make test-coverage` completed; total line coverage was about 55.8%. `service.c` remains effectively uncovered by the automated coverage report.
- `clang --analyze` over project C sources produced no analyzer warnings.
- ASAN/UBSAN full integration run against `build/mac-guest-agent-asan` passed.
- `shellcheck -S error` over shell scripts passed.
- `bash -n` over shell scripts passed.
- `actionlint` passed.
- Workflow YAML parsed cleanly.
- `plutil -lint configs/com.macos.guest-agent.plist` passed.
- `./scripts/gen-command-table.sh ./build/mac-guest-agent-universal` passed and generated docs matched tracked docs.
- `make docs/mac-guest-agent.8` produced no tracked diff.
- `leaks --atExit -- ./build/mac-guest-agent-universal --version` reported 0 leaks.

## Residual Risks / Non-findings

- I did not execute the i386 slice on Tiger/Leopard in this environment. The structural gates are strong and passed, and the project documents current 10.4/10.5 runtime evidence as Tier 1 dagger rather than current-artifact runtime proof.
- The Linux `surrogate-32bit` CI job is the clean way around "modern macOS CI cannot execute i386": it exercises portable 32-bit code under `-m32`, while `verify-legacy-slices.sh` and sabotage tests enforce Mach-O loadability. That is still not identical to a Tiger runtime, but it is substantially cleaner than requiring Intel Mac hardware.
- Live SIP-disabled `--virtio` install/uninstall was not executed on a Big Sur+ VM. The parser/state-machine tests pass and the code review found the old rollback/clobber bugs fixed, but live `csrutil` / `launchctl` / `lsof` behavior remains a manual or future PATH-shim validation area.
