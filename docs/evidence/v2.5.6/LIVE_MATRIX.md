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
