# Testing Harness — Runtime Validation on Target Macs

This is the **how-to** for running the v2.5.0+ universal binary (current `main` release — check the GitHub releases page for the version you're targeting) on a macOS version we haven't fully validated in CI (typically Tiger, Leopard, Snow Leopard, Lion, or a clean Big Sur arm64 install) and submitting evidence the maintainer can use to promote the matching row in [`COMPATIBILITY.md`](COMPATIBILITY.md) from Tier 2 (static-validated, runtime-pending) to Tier 1 (runtime-confirmed).

CI gives strong **static** guarantees — every release goes through the Mach-O verifier ([`scripts/verify-legacy-slices.sh`](../scripts/verify-legacy-slices.sh)), the symbol-baseline diff, the load-command allowlist, the LC_UNIXTHREAD-on-legacy-slices check that fixed issue #4. CI cannot give **runtime** guarantees on OS versions GitHub doesn't ship runners for. That's where this harness comes in: a contributor with an actual Tiger Mac, an actual Snow Leopard VM, an actual arm64 Big Sur install runs the harness, captures evidence, opens a PR or attaches to a GitHub issue, and the maintainer folds the row into the matrix.

## What "Tier 1" requires

| Tier | What we have | What it proves |
|---|---|---|
| **Tier 2** (static-validated) | Mach-O verifier passes for the slice that runs on this OS, symbol baseline matches, weak-imports preserved, all CI gates green | The slice is *structurally* loadable: no LC_MAIN on legacy, no forbidden dependencies, no surprise required symbols. dyld on the target should accept it. |
| **Tier 1** (runtime-confirmed) | Tier 2 **+** the binary actually runs end-to-end on real hardware/VM of the target OS, all 44 commands register, freeze/thaw cycle behaves as documented in `docs/design/FREEZE_SEMANTICS.md`, mount-dispatch matches | The slice is structurally loadable AND the host APIs the binary calls actually exist and behave as expected on that OS version. The thing we can't get from static analysis alone. |

Two harness profiles cover the gap:

- **Profile A (in-guest only)** — for VMs/Macs where you have console access but no QGA-capable host nearby (an old Tiger Mac on your desk, a VirtualBox VM that doesn't expose a serial-to-socket channel, etc.). Captures Tier-2-plus evidence: loader-safe checks, version, self-test, install side effects. Strong signal for "the binary loads and reports itself"; doesn't exercise the freeze path. Suitable for promoting from "static only" to "runtime-confirmed (no freeze)".
- **Profile B (full host-driven)** — for VMs running under PVE, libvirt, UTM, or anything else that exposes a QGA Unix socket. Captures the full Schema-2.0 evidence drop including freeze/thaw cycles and the mount-dispatch cross-check. Required for full Tier 1 promotion.

## Profile A — In-guest evidence collection

**Use when:** standalone Mac on your desk, or a VM whose hypervisor doesn't expose a QGA channel you can reach.

**Step 1 — Transfer the binary to the target Mac/VM.**

The release artifact is `mac-guest-agent` (v2.5.1+; was `mac-guest-agent-darwin-universal` for the v2.5.0 one-day window — kept as a recovery fallback in `install.sh --local`'s search list). The SHA256 changes per release — check the release page. Transfer by whatever works on the target OS:

| Target OS | Transfer methods that typically work |
|---|---|
| 10.4 Tiger / 10.5 Leopard | scp (if Remote Login is on); shared HFS+ partition; USB stick with HFS+ format; AppleTalk/AFP from a Mountain Lion-era intermediary |
| 10.6 Snow Leopard – 10.10 Yosemite | scp; Finder file-sharing; SMB share; HTTP from a `python -m SimpleHTTPServer` running on a modern Mac on the same LAN |
| 10.11 El Capitan and later | scp; direct HTTPS download from the release page if the system TLS still accepts current GitHub certs; HTTP from a `python3 -m http.server` running on a modern Mac on the same LAN |

The TLS landscape on pre-10.13 macOS is messy — GitHub's modern cipher requirements and certificate chains will reject older `curl`s. If `curl https://github.com/...` hangs or fails, fall back to transferring from a modern machine.

**Step 2 — Verify the SHA256 inside the target.**

```bash
shasum -a 256 /tmp/mac-guest-agent
# Compare against the release page's checksums.sha256
```

If the SHA doesn't match the expected, the file was corrupted in transit (HFS+ resource forks, USB FAT32 size truncation, etc.) — re-transfer before continuing. **A SHA mismatch invalidates every downstream test.**

**Step 3 — Loader-safe pre-flight (no install yet).**

These don't execute the binary; they only inspect its Mach-O headers. They confirm dyld *could* load a slice for this host without actually trying:

```bash
file /tmp/mac-guest-agent
# Expect: Mach-O universal binary with 3 architectures: [i386:...] [x86_64:...] [arm64:...]

lipo -info /tmp/mac-guest-agent
# Expect: i386 x86_64 arm64
```

If either fails, the binary itself is broken — stop and report.

**Step 4 — First execution (the load-command test).**

This is the actual issue #4 reproduction gate. If your dyld doesn't accept the binary's load commands, `--version` will SIGTRAP here. If it works, the slice that matters for your OS loaded successfully.

```bash
/tmp/mac-guest-agent --version
# Expect: mac-guest-agent <release-version> (matching the binary you downloaded)
```

On 10.6 Snow Leopard and 10.7 Lion, this is the EXACT command that crashed in v2.4.3 with `dyld: unknown required load command 0x80000028`. A clean response here is the runtime confirmation of the LC_UNIXTHREAD fix.

**Step 5 — Install + self-test.**

```bash
sudo mv /tmp/mac-guest-agent /usr/local/bin/
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
sudo launchctl list com.macos.guest-agent | grep -E '"PID"|"LastExitStatus"'
# Expect: numeric PID (not "-"), LastExitStatus = 0

sudo /usr/local/bin/mac-guest-agent --self-test-json
# Expect: agent_version = (whichever release you installed), selected_arch = (the slice dyld picked for this host),
# errors = 0, status = pass
```

**Step 6 — Run the collector script.**

```bash
sudo bash scripts/contributor-evidence-collect.sh
# Output: /tmp/mac-guest-agent-evidence.txt (and stdout)
```

The script bundles Steps 3-5 plus log tail and `launchctl list` output into one file you can transfer back to a modern machine and submit. It's deliberately bash-1-compatible (Tiger ships bash 2.05b) and doesn't depend on Python 3, jq, or curl.

**Step 7 — Redaction review (CRITICAL).**

The in-guest collector does NOT auto-redact PII the way `scripts/verify.sh` does on the host side — it has to work on Tiger where the regex surface is older. Before submitting:

- Skim the file for your guest's hostname, internal IP addresses, MAC addresses, network names, anything else identifying. Replace with `<REDACTED-HOSTNAME>`, `<REDACTED-IPV4>`, etc.
- The agent log at `/var/log/mac-guest-agent.log` may include guest-exec output if any commands ran — review the tail section carefully.
- Apple log lines (`os_log` on 10.12+) sometimes leak filesystem paths under `/Users/<your-name>` — redact those.

Once clean, transfer to a modern Mac for submission.

## Profile B — Full host-driven evidence collection

**Use when:** the VM runs under PVE, libvirt, UTM (QEMU backend), or any QEMU configuration where the QGA socket is reachable from the host. This is the path that produces full Schema-2.0 Tier 1 evidence.

This is the same flow that produced the existing [`docs/evidence/10.11.6/`](evidence/10.11.6/) drop. Short version:

**Step 1 — Install the agent inside the VM** per Profile A Step 5 above.

**Step 2 — Run `scripts/verify.sh` from the host.**

```bash
# From your modern Mac with access to the VM's QGA channel:
curl -fsSL https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh \
  -o /tmp/verify.sh \
  && chmod +x /tmp/verify.sh \
  && /tmp/verify.sh <identifier> | tee verify-output.txt
```

`<identifier>` is a numeric VMID on PVE, a domain name on libvirt, a UTM VM name on UTM. `--help` lists all flags. `verify.sh` auto-detects the transport; pass `--transport pve|libvirt|utm|qga-socket` to force one.

PII is redacted by default. Pass `--no-redact` only if you trust the audience and are self-archiving.

**Step 3 — Split the output.**

`verify-output.txt` contains a human-readable section followed by a `JSON Appendix (paste into docs/evidence/<version>/verify.json)` header and the JSON. Split at that header:

```bash
# everything before the JSON header → verify.txt
sed '/^JSON Appendix/,$d' verify-output.txt > verify.txt
# everything from the first { onward → verify.json
sed -n '/^{/,$p' verify-output.txt > verify.json
```

**Step 4 — Submit.**

Open a PR adding `docs/evidence/<version>/verify.txt` + `verify.json` + (optional) `README.md`, and update the matching row in `docs/COMPATIBILITY.md` from Tier 2 to Tier 1. Or attach the two files to a GitHub issue and the maintainer will fold it in.

## Recommended target profiles

The audit specifically called out these as the highest-value gaps:

| Target | Why it matters | Profile that fits |
|---|---|---|
| 10.4 Tiger / 10.5 Leopard, i386 slice | Oldest support floor; the only slice these dyld versions can load. Validates the entire LC_VERSION_MIN_MACOSX 10.4 + weak_import host_statistics64 + vm_stat-fallback path | Profile A (Tiger doesn't fit a PVE/libvirt/UTM workflow naturally; standalone hardware test most realistic) |
| 10.6 Snow Leopard / 10.7 Lion, x86_64 slice | Exact OSes where issue #4 manifested in v2.4.3; runtime confirmation of the LC_UNIXTHREAD recipe | Either profile; Profile B preferred for full freeze cycles |
| 11.0 Big Sur, arm64 slice | Apple Silicon floor; validates LC_BUILD_VERSION minos=11.0 + the arm64 symbol baseline added in audit wave 5 (MED-1) | Profile B (modern enough that PVE/libvirt/UTM is the natural path) |

## What "good" looks like

A successful Profile B run produces output ending in `Status: ALL CHECKS PASSED`, with the JSON appendix carrying:

- `counts: {passed: 38, failed: 0}` (or more passes if the host's `freeze-cycles` is bumped above 3)
- `in_vm_selftest.agent_version` matches the release you installed
- `in_vm_selftest.system_info.selected_arch:` the slice dyld picked for the guest (i386 for Tiger/Leopard, x86_64 for Snow Leopard through Catalina, arm64 for Big Sur+)
- `mount_dispatch_crosscheck.match: true`
- All `freeze_cycles_log` entries with `behavioural_check: "pass"`

The existing `docs/evidence/10.11.6/` is the reference. Compare your output's shape to that.

## Reporting failures

A `--version` SIGTRAP on 10.6/10.7 with any v2.5.x release would be a critical regression of the issue #4 fix. A failed `--self-test` step listing a specific missing tool or kext is a doc-update opportunity (the matrix row should reflect what's actually present). A `scripts/verify.sh` non-zero exit on freeze/thaw is a freeze-semantics issue worth a GitHub issue with the full `verify.txt` + `verify.json` attached.

Open at [github.com/mav2287/mac-guest-agent/issues](https://github.com/mav2287/mac-guest-agent/issues/new). Include the OS version, hardware/hypervisor, the exact command that failed, and the captured evidence file (Profile A or Profile B).
