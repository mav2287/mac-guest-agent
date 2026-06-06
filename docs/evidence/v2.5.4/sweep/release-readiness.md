## v2.5.4 release readiness summary — 2026-06-06 ~03:00 local

### Commits on `main` for v2.5.4 (10 total, in order)

```
4d2917d fix(load): x86_64 slice now loads on Tiger 10.4.7+ (issue #9)
e8db739 fix(channel): treat read==0 on isa-serial as EAGAIN, not EOF (issue #10)
f0fae8f chore(v2.5.4): VERSION bump + CHANGELOG + Tiger setup guide + evidence
7883573 fix(exec): bypass i386 libc execvp wrapper for absolute paths
6490a99 log(watchdog): drop Tiger-specific wording, downgrade WARN → INFO
0822407 docs(COMPATIBILITY): runtime evidence drop for v2.5.4 across 10.4-10.11
ab2cd03 docs(evidence): v2.5.4 release-readiness sweep evidence
880a435 docs(evidence): full v2.5.4 test matrix — 60+ scenarios per VM
17be907 docs(evidence): root-cause Tiger wedge to network-get-interfaces
a740cb3 docs(CHANGELOG): note v2.5.4 late-cycle additions baked under same version
```

### Fixes shipped in v2.5.4

1. **Issue #9** — x86_64 slice now loads on Tiger 10.4.7+ (Makefile flags
   + relauncher fallback + lipo-thin install)
2. **Issue #10** — `read() == 0` on isa-serial chardev correctly treated
   as `EAGAIN` (transient peer disconnect) instead of EOF (fatal device
   hangup). EOF-storm wedge mechanism gone.
3. **i386 exec bug** — i386 slice's `guest-exec` returned silent exit 127
   on every binary due to Apple's pre-10.6 i386 libc `execvp()` wrapper
   bug. New `exec_child_image()` helper bypasses it for absolute paths.
4. **Watchdog message clarity** — log line no longer claims "Tiger
   serial driver may be wedged" when firing on Leopard/SL/El Capitan
   idle cycles. Downgraded WARN → INFO.

### Test coverage achieved during the sweep

| Surface | Coverage |
|---|---|
| **VMs covered** | 4: BAM-Xserve 10.11.6, Tiger 10.4.11, Leopard 10.5.8, SL 10.6.8 |
| **Slice paths exercised** | x86_64 (BAM, Tiger, SL), i386 (Leopard) — three of three Intel slices live-validated |
| **QGA commands exercised** | 23 PVE-callable (16 whitelisted + 7 raw via socat) + guest-exec + guest-exec-status + 8 not-yet-PVE-exposed via direct probe = 33 of the 43 enabled commands directly exercised; the 10 sync/info subcommands exercised implicitly via ping/info |
| **Test scenarios per VM** | 60+ |
| **Total tests** | 240+ |
| **Codex-recommended tests** | 8 of 8 PASS |
| **Long stress** | 100 sequential pings (Tiger, BAM) — 100/100 PASS; 100 freeze/thaw cycles (all 4 VMs) — 400/400 PASS; 45+ min idle on all 4 — zero state changes |
| **Install/uninstall** | 7-scenario round-trip on BAM (install/reinstall/uninstall/install/uninstall/uninstall-when-absent/reinstall) — all PASS |
| **guest-shutdown end-to-end** | 3 of 4 VMs (Tiger 25 s, BAM 39 s, SL 38 s — full cycle: shutdown → power-off → start → LaunchDaemon auto-respawn → ping back) — all PASS |

### Compatibility chart updates

- 10.4.11 Tiger: **Tier 1†** → **Tier 1** (v2.5.4 runtime validated)
- 10.5.8 Leopard: **Tier 1†** → **Tier 1** (v2.5.4 runtime validated)
- 10.6.8 Snow Leopard: **Tier 2** → **Tier 1** (v2.5.4 runtime validated — first runtime validation of the x86_64 slice path that 10.6 is the deployment target for)
- 10.11.6 El Capitan: stays **Tier 1**, refreshed to note BAM-Xserve VM 107 v2.5.4 sweep evidence on top of the existing real-hardware verifier evidence (vit9696 PR #5)

### Known issues going into release

1. **`guest-network-get-interfaces` on Tiger 10.4** — intermittently times
   out (4-6 s, PVE rc=255), leaves the PVE-side chardev proxy state in
   "not running" until the agent LaunchDaemon plist is reloaded. **Not
   blocking** — the command is not on the freeze/backup critical path.
   Reproducible trace under `docs/evidence/v2.5.4/sweep/test-matrix.md`.
   Workaround: don't call `guest-network-get-interfaces` on Tiger
   guests; if you must, accept the recovery cost.

2. **PVE `qm guest exec` 64 KB output cap** — PVE wrapper truncates at
   64 KB regardless of agent's 16 MB `MAX_CAPTURE_SIZE`. Real workloads
   needing > 64 KB output must use raw QGA (socat) or the file-*
   command family. Documented in sweep/README.md.

3. **`--self-test-json` mode-line on stdout when invoked through nested
   SSH** — cosmetic; the JSON itself is valid when invoked directly.
   Not blocking.

### Release recommendation

**v2.5.4 is release-ready as of `a740cb3`.** Ten commits on `main`,
none reverting any prior change. All four PVE guest test VMs are healthy
and idle as of monitoring close (~02:55 2026-06-06). The release artifact
(`build/mac-guest-agent-universal`) builds clean from `make build-all`,
`make test` passes 38/0, the static legacy-slice verifier passes for
all three slices.

Next operator step: cut the GitHub release tag `v2.5.4` against
`a740cb3`, attach the universal binary, and publish the release notes
from the v2.5.4 CHANGELOG section. The dev-side `--upgrade` UX
(v2.5.3) will pick the new release automatically.
