# v2.5.6 live deploy + test matrix

Binary under test: tri-fat universal `mac-guest-agent` **v2.5.6**, host build
md5 `64a2d45f5cdb88af1ffdaeecbe5a916b` (i386 + x86_64 + arm64); i386 thin slice
md5 `d953c879c206ac1459995d4ae538801d` (byte-identical to `lipo -thin i386`).

What changed vs v2.5.5 (the binary vit9696 hit in issue #12):
- `guest-fstrim`, `guest-set-vcpus`, `guest-set-memory-blocks` **not registered**
  (no macOS equivalent; matches upstream `CONFIG_*` gating) → `CommandNotFound`.
- `guest-suspend-disk` / `-ram` / `-hybrid` **registered but disabled**
  (`enabled=0`, no QEMU/OpenCore wake path) → `CommandNotFound` on the normal path.
- Registered command count **45 → 42**.
- Version string bumped `2.5.5 → 2.5.6` (it had been left at 2.5.5 even though the
  behavior already changed — fixed so old/new binaries are distinguishable).

## Method

Each target was tested with the **same command vit9696 ran in issue #12**
(`mac-guest-agent --safe-test-json`) plus `--self-test-json`, `guest-info`, and
the full 57-case safe command battery (`tests/local-battery.sh` request set:
the read-only/idempotent commands, mutating commands in error-form or against
scratch targets, and `CommandNotFound`/`InvalidParameter` assertions for the 6
removed/disabled commands — **no** live `guest-shutdown`/`guest-suspend-*`).

Driven entirely host-side from the PVE host: the binary is pushed to the guest,
requests are piped to `mac-guest-agent --test` over one SSH session, and
responses are parsed/asserted on the host (`jq` + Python). The guests need no
`jq`/`bash` parsing — older macOS (Tiger/Leopard) ships neither, which is why an
earlier on-guest battery returned "0 of 0". The x86_64 VMs run the universal
binary; Tiger and Leopard run the **i386 thin slice** (their production install
slice — a 10.4.7+ VM otherwise grades the x86_64 slice, whose `getifaddrs` hangs
on Darwin 8).

## Results

| Target | OS / arch | Slice | `--safe-test-json` | `--self-test-json` | guest-info | Battery |
|---|---|---|---|---|---|---|
| Host (this Mac) | macOS arm64 (dev host) | arm64 | **26/0 pass** | **15/0/0 pass** | 42 reg / 39 en | `make test` all green |
| Snow Leopard VM 113 | 10.6.8 x86_64 | universal | **26/0 pass** | **14/0/0 pass** | 42 reg / 39 en | **57/57** |
| El Capitan VM 107 | 10.11 x86_64 | universal | **26/0 pass** | **16/0/0 pass** | 42 reg / 39 en | **57/57** |
| Tiger VM 111 | 10.4.11 i386 | i386 thin | **26/0 pass** | **14/0/0 pass** | 42 reg / 39 en | **57/57** |
| Leopard VM 112 | 10.5.8 i386 | i386 thin | **26/0 pass** | **14/0/0 pass** | 42 reg / 39 en | **57/57** |

`--self-test-json` pass-count varies by OS (14–16) because some checks are
platform-gated; every target reported `status:pass`, `0` failures, `0` errors.

On all four reachable targets `guest-info` reports exactly:

```
registered = 42, enabled = 39
  guest-fstrim            : not-registered
  guest-set-vcpus         : not-registered
  guest-set-memory-blocks : not-registered
  guest-suspend-disk      : disabled
  guest-suspend-ram       : disabled
  guest-suspend-hybrid    : disabled
```

The battery confirms each of the 6 returns `CommandNotFound` on call, and that
the 36 callable commands return correct data on each OS — including Tiger's
SIOCGIFCONF `guest-network-get-interfaces` (asserts `lo0` + a hardware-address
interface) and `ioreg` `guest-get-diskstats`/`guest-get-disks` paths, which exist
specifically because `getifaddrs`/`IOServiceMatching` hang in the Tiger daemon.

## Issue #12 reproduction — fixed

On v2.5.5, `--safe-test-json` reported `20 passed, 1 failed, status:fail` on
every macOS version (the `guest-fstrim` handler errored while the self-test still
expected success). On v2.5.6 the identical command reports **`26/0 status:pass`**
on arm64, Snow Leopard, El Capitan, and Tiger. The escape is now blocked by the
ship gate (`scripts/check-selftest.sh`): build and release CI, and `make test`,
all fail unless `--self-test-json` **and** `--safe-test-json` report `status:pass`
with 0 failures/errors.

## Leopard VM 112 — recovered, then passed

When the wave started, VM 112 was idle-wedged: `qm status` = running (~15% CPU)
but **both** channels dead — the QGA serial returned nothing and in-guest sshd
reset at `kex_exchange_identification` on every attempt, with no recovery after a
settle period or a `qm sendkey` nudge. This is the known legacy-guest idle-wedge
(lost-wake), unrelated to the agent or the v2.5.6 changes.

Recovered with a clean restart (operator-authorized): `qm stop 112` (safe — the
Leopard HFS+ disk is journaled on `cache=writethrough`, so the journal replays on
boot) then `qm start 112`. The guest booted to Darwin 9.8.0 (10.5.8), sshd came
up in ~20 s, and the v2.5.6 i386 battery then ran clean: `--safe-test-json`
**26/0**, `--self-test-json` **14/0/0**, `guest-info` 42/39 with all six commands
in their intended state, full battery **57/57** — including the `getifaddrs`
normal interface path (Darwin 9, not Tiger's SIOCGIFCONF special-case).

---

# Ground-truth audit (2026-06-15) — every command cross-checked against the live OS

After the count/version validation above, a deeper pass verified that each
data-returning command returns **true** data, not just well-shaped data. Every
command's output was compared field-by-field to an independent authoritative
source on the same guest (`uname`, `sw_vers`, `hostname`, `date`, `sysctl`
hw.logicalcpu/hw.memsize/vm.loadavg, `df -k`, `ifconfig`, `netstat -ibn/-rn`,
`who`, `diskutil`), backed by an independent adversarial `codex` review of every
handler. This caught four "plausible but wrong" bugs (right shape, wrong values)
that every prior shape-only test passed — see the CHANGELOG `## 2.5.6` "Fixed —
data-truth bugs" entry. All four are fixed and guarded by new `--safe-test`
invariants (safe-test 26→**30**).

## Fixed-binary coverage matrix (md5 `c60257f5…`, i386 thin `154bb944…`)

| Target | OS / slice | safe-test | ground-truth | command battery | destructive (real side effects) | daemon-side QGA |
|---|---|---|---|---|---|---|
| Host (this Mac) | arm64 | **30/0** | `make test` green | — | — | — |
| Snow Leopard 113 | 10.6.8 x86_64 | **30/0** | **23/23** | **59/59** | **12/12** | **59/59** |
| El Capitan 107 | 10.11 x86_64 | **30/0** | **23/23** | **59/59** | **12/12** | **59/59** |
| Leopard 112 | 10.5.8 i386 | **30/0** | **23/23** | **59/59** | **12/12** | **59/59** |
| Tiger 111 | 10.4.11 i386 | (see note) | (pre-fix 22/23) | — | — | — |

- **ground-truth (23 checks):** kernel-release/machine/version, hostname,
  timezone offset, wall-clock (0–1 s drift), vCPU count, RAM total
  (`blocks × block-size == hw.memsize`), cpustats count + live ticks, load,
  users, disks, fsinfo total (byte-identical to `df`), diskstats, interface
  MACs/IPs, **lo0 statistics now consistent** (`rx==tx`, `errs==0`), primary IP,
  **default + loopback routes with correct prefix**, exec pid.
- **destructive (12 checks):** file write→flush→close→reopen→read (bytes match
  on disk), read-at-EOF, seek+read, bad-handle errors, ssh authorized-keys
  add→get→remove with backup/restore, set-user-password round-trip
  (`crypted=false` base64 path).
- **daemon-side QGA:** the full 59-case battery driven over the **real ISA
  serial channel** (`socat` → `.qga` socket) with live async `guest-exec`
  polling — the production transport, not just `--test`.

## Tiger VM 111 — direct fixed-binary re-test blocked by VM transport (not the agent)

The four bug fixes are version-independent C in `cmd-network.c` / `cmd-file.c` /
`cmd-user.c`, exercised through the same code on every slice. They are proven on
the **identical i386 thin slice Tiger runs** — Leopard VM 112 passed all five
columns above (ground-truth, battery, destructive, daemon-side QGA) on that
slice — plus two other OSes and the arm64 host. Tiger's own pre-fix ground-truth
this session showed every other field already correct; the only gaps were the
lo0 statistics and route prefix, both fixed in that shared code.

A *direct* re-test on VM 111 was blocked this session by the VM's transport, not
the agent: its slirp sshd stalls on reverse-DNS and fork-storms under repeated
connections (resets at `kex_exchange_identification`), and its QGA ISA-serial
channel did not answer after a recovery reboot (the launchd daemon did not
re-attach; `/tmp` is wiped on boot, so the fixed binary could not be re-staged
over any channel). This is the documented issue-#10 / slirp fragility. The real
i386 iMac (real bridge network) is the authoritative i386 bed for a future
direct confirmation.

---

# Full re-test on the FINAL binary (all codex fixes) — every VM incl. Tiger

Binary: universal md5 `252e0f1a452683b0ab9514a5f1021cd9`, i386 thin
`10a1d3ca4ae5e553fab72748bf965ba5` (commit `d4e2f4a`, includes the codex-audit
follow-ups: route slash zero-extension, ssh fail-secure, file-open strict modes,
base64-fail handling, etc.).

| Target | safe-test | ground-truth | battery (--test) | daemon-side QGA | destructive | ssh fail-secure |
|---|---|---|---|---|---|---|
| Snow Leopard 113 | **31/0** | **23/23** | **59/59** | **59/59** | **12/12** | symlink→err, absent→empty ✓ |
| El Capitan 107 | **31/0** | **23/23** | **59/59** | **59/59** | **12/12** | (covered on 113/112) |
| Leopard 112 (i386) | **31/0** | **23/23** | **59/59** | **59/59** | **12/12** | symlink→err, absent→empty ✓ |
| Tiger 111 (i386) | — | **10/10 (QGA)** | — | **55/59 + tail 7/7** | (= Leopard slice) | (= Leopard slice) |

Tiger 111 was tested **entirely over the QGA serial channel** (SSH is unusable —
slirp reverse-DNS sshd resets): the binary was delivered with one post-boot scp
to `/private/var/tmp`, then all checks ran via `guest-exec` over QGA.
- **QGA ground-truth 10/10** against Tiger's own OS: kernel-release/version/
  hostname/vCPU/RAM/fsinfo match; **lo0 statistics consistent** (`rx==tx`,
  `errs=0` — column-shift fixed); MAC matches `netstat -ibn`; **route
  `127.0.0.0/8`** (mask `255.0.0.0`); **all IPv4 destinations full dotted quads**
  (zero-extension working). These are exactly the issue-related outputs.
- **Daemon-side battery 55/59** delivered+passed in one run; the 4 undelivered
  were the trailing error-path cases lost to the issue-#10 serial-truncation
  under sustained load (a transport limit, not a command failure). Re-run as a
  small tail battery: **7/7** (incl. `crypted=true` reject, file-open `br`/`zzz`
  reject, `CommandNotFound`, missing-`execute`). So all 59 command behaviors are
  confirmed on Tiger; only the single-shot full-battery capture is truncation-
  limited by the VM's UART.

Net: every command and every issue-related output verified on all four VMs +
the arm64 host. Tiger required splitting the battery to work within its serial
channel's throughput; the agent logic and the fixes are correct on it.
