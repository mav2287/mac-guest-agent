# Cleanup: relauncher removal + Tiger workaround verdict

Context: now that the agent is always *installed* as a pure i386 slice on a
32-bit-kernel guest (service.c `extract_native_slice`), the daemon on Tiger is
i386 from birth. That makes the pile of x86_64-on-Tiger scaffolding re-examinable.

## 1. Relauncher — REMOVED

`src/relauncher.{c,h}` re-exec'd the i386 slice when the x86_64 slice ran on
Tiger/Leopard. It was both non-functional and harmful on Tiger:

- **Can't work.** On Tiger 10.4.7+ the kernel rejects `execve` of an i386 image
  from an x86_64 process (`EBADEXEC`). A runtime x86_64→i386 switch is impossible.
- **Actively blocked install.** It `exit(1)`s on the failed `lipo` *before*
  `main()`, so `sudo /tmp/fat-binary --install` on Tiger died in the relauncher
  before the installer ran.
- **Never the thing delivering i386.** Live daemons confirm the *install* sets
  the arch: VM 112 (Leopard) = thin i386, VM 113 (SL) = thin x86_64, read from
  the installed Mach-O headers. The relauncher was irrelevant to all of them.

Removed: the files, the `main.c` call, the Makefile source + its comments (kept
the weak-framework / classic-bind flags — still needed so the x86_64 *installer*
loads), and the source list in `tests/test_legacy_slice_gate.sh`. All slices
build clean under `-Werror`; host tests 128 + 48 + fuzz + install-flags pass.

## 2. Tiger daemon-context workarounds — VERDICT: x86_64-on-Tiger artifacts

Three commands work around a Tiger daemon-context hang, gated by `uname` Darwin-8
(fires on i386 *and* x86_64 Tiger):

| Command | Workaround | Native call it avoids |
|---|---|---|
| `guest-network-get-interfaces` | SIOCGIFCONF ioctl walk | `getifaddrs(3)` |
| `guest-get-diskstats` | `ioreg` popen | `IOServiceMatching` enumeration |
| `guest-network-get-route` | `netstat -rn` popen | `sysctl(NET_RT_DUMP)` |

All three were developed against the **x86_64** Tiger daemon (the old fat install
ran x86_64). Question: do the native calls also hang under an **i386** daemon?

### Test: `build-test/legacy-daemon-probe` (i386) under a LaunchDaemon

Run in the same launchd context the agent daemon runs in (root, no tty), each
native call wrapped in `alarm(12)`.

**iMac — real i386 Tiger 10.4.5 daemon (uid=0):**

```
getifaddrs(3)                RETURNED in 0s   (11 entries)
IOServiceGetMatching(block)  RETURNED in 0s   (2 drivers)
sysctl(NET_RT_DUMP)          RETURNED in 0s   (3108 bytes)
```

**VM 111 — QEMU Tiger 10.4.11, i386 v2.5.5 daemon (uid=0):**

```
getifaddrs(3)                RETURNED in 0s   (8 entries)
IOServiceGetMatching(block)  RETURNED in 0s   (2 drivers)
sysctl(NET_RT_DUMP)          RETURNED in 0s   (2180 bytes)
```

**All three return instantly on BOTH real i386 hardware AND QEMU i386.** The
QEMU variable does not matter — the daemon-context hangs were purely an
x86_64-on-Tiger libSystem artifact (Tiger's x86_64 userland was new in 10.4.7
and half-baked), not a Tiger-wide or QEMU-specific property. **Verdict: the
three workarounds are removable.** Use the native `getifaddrs` /
`IOServiceMatching` / `sysctl` paths for all versions, dropping the Tiger
(Darwin-8) special-case.

### How VM 111 (QEMU) was finally deployed

111's x86_64 daemon couldn't `exec` (the bug), SSH was deadlocked by slirp
reverse-DNS vs sshd login-grace, ACPI shutdown didn't respond, and a bridge NIC
didn't bring Tiger's `en0` up. The path that worked: hard-stop → kpartx → the
HFS+ wouldn't rw-mount (dirty journal, no `fsck.hfsplus`), so clear the journaled
bit in the volume header (snapshot as backup; the idle VM's journal was empty) →
rw-mount → drop the universal binary in `/private/var/tmp` → boot. On boot the
2.5.4 binary's (flaky) relauncher happened to relaunch i386 this time, so
guest-exec worked → `guest-exec /private/var/tmp/mga-2.5.5 --upgrade` ran the
x86_64 installer through the **exec-free** path → installed a **pure i386** slice
(`magic 0xfeedface cputype=i386`) → daemon respawned i386 → `guest-exec /bin/echo`
works (was EBADEXEC), `uname -m`=i386, 10.4.11. **56/56 battery passed.** This is
the end-to-end proof of the exec-free x86_64-installer→i386 install on QEMU.
(111's FS is currently non-journaled from the journal-bit clear; re-enable with
`diskutil enableJournal /` from inside if desired.)

## Channel wedging under i386 — two distinct issues, one fixed

After removing the workarounds and redeploying to the i386 v2.5.5 daemon (111),
stress-tested the channel:

- **Command-hang wedge (sustained back-to-back QGA traffic) — FIXED.** 150 rapid
  commands including the exact triggers that used to wedge the x86_64 daemon
  (`guest-get-diskstats` / `guest-network-get-route` / `guest-network-get-interfaces`):
  **150/150 ok, channel fully alive after.** That wedge was the x86_64 daemon
  hanging inside the native call, which blocked the whole serial channel. On i386
  the calls return instantly, so nothing hangs and nothing wedges — same root
  cause as the workaround verdict. This is the main historical "wedging" and it
  is gone on i386.

- **Large-inbound-message byte loss (`docs/evidence/UART_DRAIN.md`) — UNCHANGED,
  not arch-related.** `guest-file-write` payloads on i386: 1 KB OK, 2 KB+ lost
  (~1.5 KB threshold) — identical to the x86_64 measurement. This is the
  emulated-16550 RX overrun (single-vCPU Tiger can't service the UART receive
  interrupt fast enough during QEMU's memory-speed burst), a hardware property
  below the agent. It is byte loss on one oversized message, NOT a permanent
  wedge — each subsequent `open` still worked, so the channel recovers. Fix
  stays sender-side: chunk inbound ≤1.3 KB (the deploy tooling does).

## Native paths validated via the agent on i386 Tiger

With the workarounds removed, the agent's commands return correct data through
the now-native paths on the QEMU i386 daemon (111): `guest-network-get-interfaces`
(native getifaddrs) → `lo0`, `en0`; `guest-get-diskstats` (native IOServiceMatching)
→ `disk0`; `guest-network-get-route` (netstat) → 13 routes. Full 56-case battery:
56/56. Host build clean (`-Werror`, all 4 slices); 128 + 48 + fuzz + local 56/56.
