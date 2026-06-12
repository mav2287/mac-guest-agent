# v2.5.5 live deploy + test matrix

Binary under test: tri-fat `dist/mac-guest-agent` v2.5.5,
md5 `4d482a4fbc1b30f1e61e0ef39ee20fd9` (i386 + x86_64 + arm64), byte-identical
artifact deployed to every target.

Method: deployed via the agent's own `--upgrade` (which exercises the new
atomic stage-temp + `rename()` placement), then exercised the full 56-case
command battery (`tests/exhaustive-commands.sh`) against each target's
**real** system through in-guest `--test` mode, plus an 11-point check of the
v2.5.5 behavior changes. Battery responses parsed with a streaming JSON
decoder (the `guest-sync-delimited` reply carries a leading `0xFF`, which a
naive line splitter drops — that is a harness detail, not an agent fault).

## Results

| Target | OS / arch | Channel | Deploy | Battery | New-behavior |
|---|---|---|---|---|---|
| Leopard VM 112 | 10.5.8 x86_64 | QGA (slirp) | 2.5.4 → **2.5.5** | **56/56** | **11/11** |
| Snow Leopard VM 113 | 10.6.8 x86_64 | QGA (slirp) | 2.5.4 → **2.5.5** | **56/56** | **11/11** |
| BAM-Xserve VM 107 | 10.11 El Cap x86_64 | QGA (bridge) | 2.5.4 → **2.5.5** | **56/56** | **11/11** |
| iMac (physical) | 10.4.5 Tiger **i386** | SSH (no QGA) | 2.5.3 → **2.5.5** | **56/56** | via battery |
| Tiger VM 111 | 10.4.11 → i386 | QGA (slirp) | x86_64 2.5.4 → **i386 2.5.5** | **56/56** | native paths OK |

VM 111 was deployed by clearing the HFS+ journal bit to force an offline rw-mount
(no `fsck.hfsplus` on the host), dropping the universal binary in `/private/var/tmp`,
booting, and triggering the binary's own exec-free `--upgrade` over QGA — which
installed a pure i386 slice (the x86_64-installer→i386 path). guest-exec then works
(was EBADEXEC), 56/56 battery, native getifaddrs/IOServiceMatching/netstat all
return correct data, and a 150-command stress test does not wedge the channel.
See `cleanup-relauncher-and-workarounds.md`.

Host (arm64) pre-flight: `make build-all` clean under `-Werror` for all four
slices; 128 unit/proactive + fuzz tests pass; local 56/56 battery.

## Install-arch root-cause fix (v2.5.5) — one universal binary installs i386 on Tiger

Discovered while deploying to the Tiger VM: a Tiger 10.4.7+ **VM** runs the
universal binary's **x86_64** slice (XNU grades it higher on EM64T), but Tiger's
`/bin` + `/usr/bin` are i386/ppc only — so the x86_64 daemon's `execve` of any of
them fails with `EBADEXEC`. Proven on 111: exec of the agent itself (has an
x86_64 slice) → exit 0; exec of `/bin/echo` (i386+ppc) → `EBADEXEC`. Every system
tool (`cp`/`chmod`/`launchctl`/`ps`/`sh`/`curl`) fails the same way. This breaks
`guest-exec`/`guest-shutdown` and any exec-dependent install step. (The earlier
"exhaustive" battery missed it because `guest-exec … @@ ok` only checked that a
**pid** came back, not the **exit status** — fixed assertion pending.)

Fix (all in the binary, one command for every VM):
- **In-process slice extraction**: install/upgrade parses the Mach-O fat header
  and writes exactly the `uname -m` slice (i386 on Tiger) — verified
  byte-identical to `lipo -thin i386`, no `lipo`/`cp` child. Never leaves a fat
  binary that would grade up to x86_64.
- **Whole install is exec-free** (works even when the installer runs x86_64 on
  Tiger): placement = extract + `rename()`; backup = `read`/`write` + `fchmod`;
  daemon restart = `kill()` the stale daemon so the plist's `KeepAlive` respawns
  the freshly-placed i386 binary; running-check = `sysctl(KERN_PROC)`. Modern
  guests keep the `launchctl` path; syscall path is the automatic fallback.

Validation: extractor byte-identical to `lipo` for i386/x86_64/arm64; syscall
copy + `sysctl` scan unit-tested; 128+48 host tests + fuzz pass; **the real i386
iMac runs v2.5.5 i386 from the same universal binary + `--upgrade`** (end-to-end
on actual Tiger hardware, where the installer runs i386 and the normal path is
used).

## Tiger VM 111 — deploy blocked by the VM's environment (not the fix)

Every channel to 111 is unusable for a deploy, all VM-specific:
- **QGA guest-exec** — arch-broken (x86_64 daemon can't exec i386 `curl`/`sh`).
- **QGA file-write push** — ~590 chunked writes wedges Tiger's serial channel.
- **SSH/scp** — sshd accepts the TCP connection but sends no banner and drops it
  after ~74s: the slirp reverse-DNS lookup on the client IP stalls past sshd's
  login grace. (The iMac avoids this — real bridge network, so SSH/scp work.)
- **Offline-mount** — needs a clean shutdown; ACPI powerdown doesn't respond on
  this VM even with the post-reset modal dialogs cleared.

Next option (pending operator decision): give 111 a bridge NIC like the other
VMs so the documented scp/curl + `--upgrade` flow works, then validate the
exec-free i386 self-install on the x86_64-graded VM. The fix is already proven on
real i386 Tiger (iMac); 111 would additionally exercise the exec-free fallback.

## New-behavior checks (11) — all PASS on 112 / 113 / 107

1. `guest-suspend-ram` → `CommandNotFound` (gated, normal path)
2. `guest-suspend-hybrid` → `CommandNotFound`
3. `guest-fstrim` → `GenericError` (honest; TRIM is automatic on macOS)
4. `guest-set-time` (argless) → `GenericError` (no hwclock equivalent)
5. `guest-set-vcpus` → `CommandNotFound` (already disabled, regression guard)
6. `guest-get-memory-blocks` → every block `online:true, can-offline:false`
7. `guest-network-get-interfaces` → `lo0` present with `127.0.0.1` (match Linux)
8. `guest-info`: `guest-suspend-disk` `enabled:true` (still works)
9. `guest-info`: `guest-suspend-ram` `enabled:false`
10. `guest-info`: `guest-suspend-hybrid` `enabled:false`
11. `guest-info`: `version == 2.5.5`

## iMac — real i386 Tiger validation

The physical iMac (Tiger 10.4.5, `xnu-792 RELEASE_I386`, libSystem ppc64/i386/ppc
with **no x86_64 userland**) is the only true i386 bed — the Tiger *VM* grades
the x86_64 slice higher and runs that. Findings on the iMac:

- `--upgrade` ran the new path: _"Staged binary (lipo-thin i386) … Installed
  binary atomically -> /usr/local/bin/mac-guest-agent"_. Installed file is
  `Non-fat … architecture: i386` — confirms atomic placement + lipo-thin on
  real i386 hardware (the ETXTBSY-prone path).
- 56/56 battery against the real system.
- Tiger daemon-context code paths return real data in `--test`:
  `guest-network-get-interfaces` (SIOCGIFCONF replacement for the hanging
  `getifaddrs`) → `lo0` + physical interfaces; `guest-get-diskstats`
  (`ioreg` popen replacement for the wedging `IOServiceMatching`) → `disk0`.

  Caveat: `--test` runs foreground, not under launchd, so it validates the
  i386 binary + the SIOCGIFCONF/ioreg logic but does not reproduce the
  launchd-daemon hang those paths exist to avoid. The daemon-context proof
  requires Tiger VM 111's live QGA channel (pending).

## Tiger VM 111 — pending

QGA channel hard-wedged on the old 2.5.4 binary (issue-#10 serial wedge;
`guest-ping` and `guest-sync-delimited` both dead; VM healthy at ~8.5% CPU,
4h+ uptime). In-guest sshd (hostfwd PVE:22111→22) is reachable at TCP level
but slow to complete the banner exchange. Deploy of v2.5.5 in progress via
SSH; will record battery + daemon-context (live QGA) results once on.
