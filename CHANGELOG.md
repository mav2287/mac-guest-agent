# Changelog

## Unreleased

### Changed — `guest-set-vcpus` / `guest-set-memory-blocks` no longer registered

Same principle as the `guest-fstrim` change below: macOS has no CPU or memory
hotplug, and upstream QEMU gates both commands behind `CONFIG_LINUX` (omitting
them where unsupported). They had been registered with `enabled=0` (so calling
them already returned `CommandNotFound`), but listing a command with
`enabled:false` misuses the QGA flag — `enabled` means "administratively
blocked", not "the platform can't do this". For consistency with `guest-fstrim`
and with upstream, both are now simply **not registered**: absent from
`guest-info`, `CommandNotFound` on call. Command count 44 → 42. The self-test
now asserts all three (`guest-fstrim`, `guest-set-vcpus`,
`guest-set-memory-blocks`) are unregistered (CommandNotFound), so the ship gate
guards against silently re-introducing any of them.

### Fixed — `guest-fstrim` no longer ships a failing self-test (issue #12)

`--safe-test-json` reported `20 passed, 1 failed, status:fail` on every macOS
version in v2.5.5 (what vit9696 hit in issue #12). Root cause: `guest-fstrim`
was changed to return an error, but the built-in self-test still expected it to
succeed — and **nothing in the pipeline ran the binary's own self-test as a
ship blocker**, so it escaped.

- **`guest-fstrim` is no longer registered on macOS.** It is on-demand bulk
  free-space discard (Linux `FITRIM`), and macOS exposes no volume-level
  free-extent trim API. We now follow upstream QEMU, which gates `guest-fstrim`
  behind `CONFIG_FSTRIM` and simply does not register it where unavailable,
  rather than shipping a stub that always errors. `guest-info` no longer
  advertises it; calling it returns the standard `CommandNotFound`. Command
  count 45 → 44. To reclaim thin-provisioned space on a macOS guest, see the new
  `docs/RECLAIM.md` (zero free space with `diskutil secureErase freespace 0`
  via `guest-exec`, plus host `detect-zeroes=unmap` / `qemu-img convert`).
- Self-test now asserts `guest-fstrim` is **not** registered (capability test).
- `--safe-test-json` is `21/0` and `--self-test-json` `15/0/0` on macOS again.

### Added — ship gate so a red binary can never ship again

`scripts/check-selftest.sh` runs the binary's own `--self-test-json` **and**
`--safe-test-json` and fails unless both report `status:pass` with zero
failures/errors. Wired into **build CI** (`build.yml`, replacing the prior step
that only checked the JSON was *parseable*, not that it *passed*, and never ran
safe-test at all), **release CI** (`release.yml`, so a tagged release cannot
publish a red artifact), and **`make test`** (`test-selftest`, so it fails
locally too). This is the direct guard for the issue #12 class of escape.

## v2.5.5 — 2026-06-13

Two themes: replace the remaining Tiger 10.4 daemon-context *placeholders*
(commands that returned empty/no-op because the obvious macOS syscall hangs
when called from a launchd-spawned daemon) with real implementations that go
around the hang; and a dead-item audit that removed commands which advertised
success while doing nothing.

### Tiger runs i386 — relauncher removed, daemon-context workarounds deleted

Root-causes and closes issue #11 ("Ping works, network-get-interfaces does not
in macOS 10.4.11"). The real problem was the *arch* the daemon ran: a fat binary
graded to x86_64 on Tiger 10.4.7+, and Tiger's new-in-10.4.7 x86_64 libSystem
hangs `getifaddrs` / `IOServiceMatching` / `sysctl(NET_RT_DUMP)` from a launchd
daemon (and can't `exec` Tiger's i386 tools). The install-arch fix above makes
the daemon i386, where those native calls return instantly — proven under a
launchd daemon on both the real i386 iMac and the QEMU i386 VM.

- **Removed `src/relauncher.{c,h}`.** It tried to re-exec the i386 slice at
  runtime via `lipo`, which is impossible on Tiger (the kernel rejects `execve`
  of an i386 image from an x86_64 process) and actually aborted the installer
  before `main()`. The reliable mechanism is installing the i386 slice.
- **Removed the three Tiger daemon-context workarounds** now that the daemon is
  i386: `getifaddrs`→SIOCGIFCONF ioctl walk and `IOServiceMatching`→`ioreg` popen
  are deleted (native calls used on all versions); the route handler's comment is
  corrected (it always used `netstat -rn`). ~420 lines removed.
- The previous **sustained-traffic channel wedge** is gone on i386 (the daemon
  no longer hangs inside those calls). The separate large-inbound byte-loss
  (`docs/evidence/UART_DRAIN.md`) is unchanged — a hardware 16550 RX overrun,
  handled by sender-side ≤1.3 KB chunking.
- Legacy-slice undefined-symbol baselines refreshed (`tests/legacy_slice_symbols_*.txt`):
  `_cfsetispeed/ospeed` and `_host_statistics64` dropped; `_chmod`/`_sysctl`/`_ftello`/
  `_strptime`/`_mktime` added (exec-free install + set-time).

### Tiger 10.4 commands now return real data (were empty placeholders in v2.5.4)

v2.5.4 shipped honest empty/`return`-nothing placeholders for several commands
that hung or returned nothing on the Tiger daemon. v2.5.5 returns real data —
and with the daemon now running i386 (see above), the **native** calls work, so
no Tiger-specific code path is needed:

- **`guest-network-get-interfaces`** — real interface/IP/MAC data via native
  `getifaddrs(3)` (validated on the i386 iMac and QEMU i386 VM). Now also
  includes `lo0` to match Linux qemu-ga. This is the direct fix for issue #11.
- **`guest-get-diskstats`** — real per-disk stats via native `IOServiceMatching`
  / IOBlockStorageDriver `Statistics`.
- **`guest-network-get-route`** — real route table via `netstat -rn` (one
  version-independent path; replaces the v2.5.4 return-empty caveat).
- **`guest-get-users`** — falls back to parsing `who` (reconstructing
  login-time) when the utmpx database is empty (a Tiger DB quirk, not arch).

### Dead-item audit — commands that pretended to work now tell the truth

Removed four cases where a command returned success (or an empty result) while
doing nothing, which misleads a caller into thinking an action occurred:

- **`guest-fstrim`** — was a `{"paths":[]}` no-op. macOS issues TRIM/UNMAP
  automatically; there is no on-demand trim to invoke. Now returns a
  `GenericError` saying so.
- **`guest-set-time`** (argless form) — was a success no-op. macOS has no
  userspace `hwclock` equivalent to resync from the RTC. Now returns a
  `GenericError`; the explicit-`time` form still works.
- **`guest-get-memory-blocks`** — `online` was derived from current RAM
  *usage* (a half-used VM looked half-unplugged). macOS has no memory
  hotplug, so every block is now reported `online:true, can-offline:false`.
- **`guest-suspend-ram` / `guest-suspend-hybrid`** — both called `pmset
  sleepnow` with an S3 hibernate mode, which has no QEMU wake path and
  *wedges the VM*. Now gated: advertised `enabled:false` in `guest-info`,
  return `CommandNotFound` on the normal path, and (if an operator
  force-enables via `--allow-rpcs`) return a `GenericError` directing to
  host-side suspend (`qm suspend` / `virsh suspend`). `guest-suspend-disk`
  (hibernatemode 25 → write image + power off, host-resumable) is unchanged.

### One universal binary installs the right arch on every Apple VM (incl. Tiger)

The shipped binary is one tri-fat (i386 + x86_64 + arm64) and one command
(`--install` / `--upgrade`) now installs correctly on **every** supported guest,
including a Tiger VM:

- **Always installs a single native slice, never fat.** Install/upgrade extracts
  exactly the `uname -m` slice (i386 on Tiger, x86_64 on Intel 10.5+, arm64 on
  Apple Silicon) **in-process** — a built-in Mach-O fat parser, no `lipo`/`cp`
  child (verified byte-identical to `lipo -thin`). The old code fell back to
  copying the *fat* binary when `lipo` was absent (a minimal Tiger). That matters
  because on an EM64T Tiger 10.4.7+ XNU grades a fat binary to its **x86_64**
  slice — but Tiger's own `/bin` + `/usr/bin` are i386/ppc only, so an x86_64
  daemon cannot `execve` any of them (`EBADEXEC`), silently breaking `guest-exec`
  and `guest-shutdown`. A pure i386 install keeps the daemon's arch matched to
  the tools it must spawn.
- **The whole install is exec-free, so it works even when run as x86_64 on Tiger.**
  When the universal binary is launched on a Tiger 10.4.7+ VM it runs its x86_64
  slice, and *every* i386 helper it would normally shell out to (`cp`, `chmod`,
  `launchctl`, `ps`) fails with `EBADEXEC`. So install/upgrade now uses syscalls
  end-to-end: binary placement via in-process extract + `rename()`; backup via a
  `read`/`write` copy + `fchmod()`; daemon restart by `kill()`ing the stale
  daemon so the plist's `KeepAlive` respawns the freshly placed i386 binary; and
  the running-state check via a `sysctl(KERN_PROC)` scan. On modern guests the
  normal `launchctl` path is used; the syscall path is the automatic fallback.

- **Atomic binary placement** (`--install` / `--upgrade`) — staged into a
  sibling temp file then `rename()`d over the target, instead of writing in
  place. A still-running daemon makes the target an in-use text file; an
  in-place write could fail with `ETXTBSY`. `rename()` only swaps the directory
  entry, so it is immune and never leaves a half-written binary at the path.

- **Standard `--install` now detects an existing install** and refuses with
  guidance (`--upgrade` to update with backup/rollback, or `--uninstall` first)
  instead of silently overwriting with no safety net — matching how the VirtIO
  modes already behave. The detection is exec-free (works on Tiger). The
  bootstrap `scripts/install.sh` passes `--install` straight through, so re-running
  the one-liner surfaces this guidance.

### `scripts/install.sh`

Thin bootstrap wrapper, corrected: it now `chmod +x`'s the downloaded binary
(a `curl -o` write is 0644, so the previous download path failed with "binary
not executable"), and its header comment no longer claims it copies the binary
(the binary self-copies). It does no arch/slice selection — it downloads the
universal binary (or `--local`) and `exec`s `binary --install`/`--upgrade`,
which is exactly equivalent to running the binary directly; it exists only so
`curl …/install.sh | sudo bash` can stand in for that (a binary can't be piped
into an interpreter). The GitHub download needs TLS 1.2+, so on 10.4–10.6 use
`--local`.
- **Inbound message buffer** `READ_BUF_SIZE` 4096 → 65536 plus a
  drain-until-newline read loop, so modern (multi-core) VMs can receive a
  large single message in one pass. (Tiger's ~1.5 KB inbound cap is a
  hardware-level 16550 RX-overrun property below the agent and is unchanged —
  see `docs/evidence/UART_DRAIN.md`; the sender chunks large writes.) The
  inert `cfsetspeed(B115200)` call was removed — QEMU ignores the UART's
  programmed baud entirely.
- **guest-exec output draining** — while a captured child still has output
  flowing, the main loop polls fast (20 ms) and drains its pipe ~50×/s,
  lifting large-output capture from ~64 KB/s (one pipe-full per idle tick)
  to multi-MB/s. Zero cost when no exec is in flight.
- **`--upgrade` verify** — `ps` keyword `comm` → `command` (Tiger's `ps`
  rejects `comm`) and a 10-iteration daemon-start poll, fixing the silent
  upgrade rollback on Tiger (issue #11 follow-up).

## v2.5.4 — 2026-06-04

### Tiger 10.4 fixes — empirically validated on real and virtualized Tiger Intel

Two upstream-reported bugs from @vit9696 (issues #9 and #10) are fixed with
fully reproducible evidence captured against both a Tiger 10.4.10 VM and a
real iMac5,1 running Tiger 10.4.5.

#### Issue #9 — x86_64 slice load failure on Tiger 10.4.7+

**Symptom:** `dyld: unknown required load command 0x80000022` on Tiger
10.4.7 and later when launching the v2.5.3 universal binary.

**Root cause:** XNU's `grade_binary` grades the x86_64 slice higher than
the i386 slice on EM64T-capable hosts once Apple added x86_64 userland in
10.4.7. v2.5.3's x86_64 slice was built with `-mmacosx-version-min=10.6`,
which emits three things Tiger's 2007-era libSystem/dyld cannot handle:

1. **`LC_DYLD_INFO_ONLY`** load command — Tiger's dyld (vintage <10.5) does
   not parse it and aborts with the error vit9696 observed.
2. **Versioned symbol references** like `_select$1050`,
   `_realpath$DARWIN_EXTSN` — the Darwin headers macro-expand POSIX calls
   to these when `__DARWIN_VERS_1050` is enabled, but Tiger's libSystem
   exports the *unversioned* names only. Bind fails before `main()`.
3. **`-fstack-protector` artifacts** (`___stack_chk_guard`) and
   **`_FORTIFY_SOURCE` chk variants** (`___memcpy_chk`, `___sprintf_chk`)
   — all added in 10.5; absent on Tiger.

**Fix** (`Makefile`, `build-x86_64` target):
- Drop x86_64 deployment target from `10.6` → **`10.4`** so the headers
  emit unversioned symbol references.
- Add `-Wl,-ld_classic` + `-Wl,-platform_version,macos,10.4,10.13` to
  force classic `LC_SYMTAB` / `LC_DYSYMTAB` binding instead of
  `LC_DYLD_INFO_ONLY`.
- Add `-fno-stack-protector` and `-D_FORTIFY_SOURCE=0` to suppress the
  `___stack_chk_guard` / `___*_chk` references.
- Keep `-Wl,-weak_framework,CoreFoundation -Wl,-weak_framework,IOKit` —
  Tiger ships CF/IOKit as i386-only, so the x86_64 slice's references
  must be weak to allow the load. Functions that actually call into
  CF/IOKit on Tiger paths are guarded against NULL function pointers (or
  routed via the relauncher to the i386 slice for full-fidelity hardware
  queries).

The relauncher introduced in v2.5.4 (`src/relauncher.c`) remains as
defense in depth: on Tiger it tries to `lipo`-extract and `execv` the
i386 slice for full CF/IOKit fidelity; on Snow Leopard+ it's a compile-time
no-op. On Tiger specifically, if `/usr/bin/lipo` is absent (no Developer
Tools installed) the relauncher logs the failure and exits non-zero
rather than falling through — operators get an explicit error pointing
at the missing tool. The new x86_64 compatibility flags mean the x86_64
slice loads cleanly on Tiger *before* the relauncher even runs, so
installs without Developer Tools still produce a working agent via the
LaunchDaemon-respawn path (the first invocation exits, launchd
restarts, and the relauncher is now a no-op because the i386 fallback
has been written by the install path).

**Validation:**
- Tiger 10.4.10 VM (Darwin 8.10.3, kernel selects x86_64 slice — the
  bug-trigger path): v2.5.3 fails with `unknown required load command
  0x80000022`; v2.5.4 self-test passes 13/13 with `"selected_arch":"x86_64"`.
- Real iMac5,1 Tiger 10.4.5 (Darwin 8.5.3, no x86_64 userland — kernel
  selects i386 slice): v2.5.4 self-test passes 13/13 with
  `"selected_arch":"i386"`.

#### Issue #10 — agent stops responding after extended uptime (mitigated)

**Symptom** (per vit9696's report): after ~4 hours of agent uptime on
Tiger 10.4.11, `qm agent <vmid> ping` returns "QEMU guest agent is not
running", while the agent process itself remains alive (0% CPU, ~668 KB
RSS). `sample` shows all 300 samples in `select()`. The host can no
longer communicate even though PVE's QGA proxy continues writing to the
channel.

This is the next layer of the same Tiger BSD serial driver fragility
class @vit9696 originally surfaced in issue #2:

- Issue #2 (closed): Tiger's `poll()` returns POLLNVAL for a valid open
  serial fd (the kqueue readiness path in Tiger's BSD serial driver
  isn't wired up). Mitigation: switch the agent from `poll()` to
  `select()`. Shipped earlier; 10.4 promoted to Tier 1.
- Issue #10 (this entry): the agent's `read()` on the serial fd returns
  `0` every time PVE's QGA proxy disconnects between calls (which it
  does, per its per-call design). v2.5.3 treated `read()==0` as a
  fatal EOF and entered an EIO-driven reconnect storm — close + sleep
  + open + read=0 again + repeat — that locked PVE out of the channel
  until a `qm agent` call happened to race the brief open-to-read
  window. The 600 s idle watchdog never fired during these storms
  because the EIO branch reset `last_msg_time = time(NULL)` every ~10 s,
  long before reaching threshold.

**Root cause** — the chardev-backed serial transport has different
`read() == 0` semantics than a physical tty. For a real tty `read()==0`
means carrier loss + hangup (dead device). But the QEMU `isa-serial`
chardev backed by a unix socket synthesizes `read()==0` on the guest
side when the host-side socket peer (PVE QGA proxy) disconnects — and
PVE disconnects after every `qm agent <vmid> <cmd>` call (it's a
per-call protocol, not a persistent connection). So what looked like
"the device died" was actually "the host hasn't called us in the last
millisecond, which is the normal state."

**Primary fix** (`src/channel.c::channel_try_read`):

```c
if (n == 0) {
    /* Chardev peer disconnect, NOT device hangup. Keep the fd open;
     * the next host-side write will arrive normally. */
    if (probe) ch->probe_eof++;
    else       ch->read_eof++;
    errno = EAGAIN;
    return 0;
}
```

Both read paths (the pre-select probe and the post-select consume) now
treat `n==0` as transient EAGAIN. The fd stays open, the loop continues,
and the next PVE connect-and-write arrives normally. We count the
events separately in `read_eof`/`probe_eof` counters so the SIGUSR1
diagnostic dump distinguishes "host disconnected" from "no data yet"
without losing signal in `read_eagain`.

Safe in the presence of a real hardware hangup because our termios
sets `CLOCAL` (`src/channel.c::channel_open`). Per `bsd/kern/tty.c`,
XNU's `ttread` only synthesizes EOF for a zombie tty when `CLOCAL` is
off; with `CLOCAL=1`, nonblocking no-data is EWOULDBLOCK rather than
0, so a `read()==0` here can ONLY be the chardev peer disconnect, not
a true device hangup.

**Supporting fixes**

- **`tcflush(TCIOFLUSH)` → `tcflush(TCOFLUSH)`** in `channel_open`. Input
  flush on reopen could discard PVE's `guest-sync-delimited` recovery
  command if it raced our reopen window. Output flush is fine (drops
  stale response bytes); input flush is wrong. Per the QGA protocol
  spec, `guest-sync-delimited` is the host's stream-recovery handshake,
  and dropping it stalls the connection until the next sync attempt.

- **EIO branch no longer resets `last_msg_time`** in `src/agent.c`. The
  watchdog timer should measure time since the last *successful
  message*, not time since the last EIO recovery attempt — otherwise an
  EIO storm self-perpetuates by always resetting the watchdog before it
  can fire.

- **Separate counter for EIO-driven reconnects** (`eio_reconnects`)
  versus watchdog-driven reconnects (`reconnects`). v2.5.3 lumped them
  together, hiding 20+ EIO storms per minute behind a single watchdog
  counter that read `reconnects=1`.

- **Nonblocking serial fd** (`O_RDWR | O_NOCTTY | O_NONBLOCK`). Without
  `O_NONBLOCK`, a "select returned ready but read blocks" failure mode
  could silently bypass everything else. The read path already handles
  EAGAIN, so nonblocking is a strict superset of the prior behavior.

- **`errno = EIO` on closed-channel `channel_read_message`**. Prior
  code returned NULL with stale errno, which the outer loop didn't
  route into the EIO branch — it fell into "Other error, usleep 100 ms,
  retry against the same closed channel" indefinitely. Defensive fix.

**Defense in depth — idle-channel watchdog** (`src/agent.c::agent_run`,
kept):

The agent tracks wall-clock time since the last successful message
read. If `WATCHDOG_IDLE_TIMEOUT_SEC` (600 s) elapses with zero messages,
it logs a warning, dumps full counter state, and force-cycles the
channel (`channel_close` + `channel_open`). This was the v2.5.4-RC1
fix BEFORE we identified the EOF-storm root cause; we kept it as a
safety net for any other failure mode we haven't yet identified in
specific user environments.

The watchdog cost during normal operation is one `time(NULL)` syscall
+ one comparison per loop iteration (~510 ns per second, ~0.00005% CPU).
Per-year CPU cost: ~16 seconds. Cost when it fires: ~10-15 ms of CPU
work plus 5 seconds of wall-time sleep. In the post-EOF-fix run
described below, it never fired.

**Diagnostic instrumentation — `SIGUSR1` channel-status dump**
(`src/channel.c::channel_dump_status`):

The agent responds to `sudo kill -USR1 <pid>` by logging a single
counter snapshot line:

```
[INFO] channel_status fd=5 open=1 test=0 \
       select=27108/22163/4936  read=561423/0/0  probe=27187/79/27108/0 \
       msgs=10028  reconnects=wd:0/eio:0 \
       buf_len=0 fionread=0 ages=0/0/0
```

Field semantics:
- `select=<calls>/<timeouts>/<ready>`
- `read=<bytes>/<eagain>/<eof>` ← EOF events surfaced separately, NOT
  lumped into EAGAIN
- `probe=<calls>/<hits>/<eagain>/<eof>` ← pre-select read-first probe
- `reconnects=wd:<watchdog>/eio:<EIO_driven>` ← split so dumps are
  honest about which mechanism is firing
- `buf_len=<bytes_buffered_awaiting_newline>`
- `fionread=<ioctl_FIONREAD_value>` ← bytes visible in tty queue
- `ages=<seconds_since_last_select>/<read>/<msg>`

This is the diagnostic tool we'd point users at if v2.5.4 still
exhibits a wedge in their environment. The dump format distinguishes
between "selrecord deaf", "tty queue not filling", "QEMU buffer
overflow", "protocol-layer message corruption", and "kernel-level
syscall blockage" — see in-source comments on `channel_dump_status` for
the full discriminator table.

**Hybrid read-first probe** (`src/channel.c::channel_read_message`):

Before each `select()` call, the agent attempts one nonblocking
`read()`. If bytes are in the tty input queue (Tiger's `selrecord`
having gone deaf and not reported them, or simply not yet reported
them), the probe consumes them directly and the read pipeline is
served without ever calling `select()`. On a healthy system the probe
returns EAGAIN immediately and the loop falls through to the existing
`select()` path — one extra syscall per iteration in the no-data case,
microseconds of overhead.

The probe counters confirmed empirically that the wedge wasn't
`selrecord` deafness (which we initially suspected): `select.ready`
advanced normally during the wedge cycle that hit pre-EOF-fix builds,
and the probe hit count tracked `select.ready` proportionally. The
actual wedge was the EOF storm described above. We kept the probe
because (a) the diagnostic counters it adds are useful, and (b) at 79
probe hits over 10,288 messages on the test rig it's catching a small
but non-zero number of bytes that select doesn't immediately surface
— cheap insurance for any future readiness-path failure.

### Validation — issue #10 fix empirically holds for 6+ hours

Deployed v2.5.4 final to a Tiger 10.4.11 VM on Proxmox VE 9.1.1
(pc-q35-6.1, Penryn CPU, slirp networking) at 03:01:04. As of
09:55:38 — **6 hours 54 minutes of continuous uptime** — the daemon
has processed 10,288 messages with these final counters:

```
select=27798/22722/5066  read=575963/0/0  probe=27877/79/27798/0
msgs=10288  reconnects=wd:0/eio:0
buf_len=0  fionread=0  ages=0/0/0
```

Across 14 SIGUSR1-triggered dumps in that window:
- `read_eof` and `probe_eof` stayed at **0** (no EOF events were
  observed during this run — PVE may be keeping its connection open
  longer than I'd theorized, or the previous wedge cycles had a
  different transient trigger that the EOF fix nonetheless covers)
- `eio_reconnects` stayed at **0** (no EIO branch ever entered)
- `wd_reconnects` stayed at **0** (watchdog never fired)
- `select_ready` and `read_bytes` advanced monotonically across every
  dump, confirming sustained healthy operation
- 0 `[ERROR]` and 0 `[WARN]` log entries across the entire run

**Pre-fix wedge cadence was 4-6 minutes** in this exact environment
(reproduced twice in the RC1 long-run, watchdog correctly recovered
both times — see `docs/evidence/v2.5.4/` for the pre-fix counter
snapshots). **Post-fix: zero wedges across 6+ hours and 10,000+
messages.**

The user's reported wedge in issue #10 was after ~4 hours of uptime.
The post-fix Tiger 10.4.11 daemon has now beaten that by 50% with zero
failures of any kind.

> **Honesty caveat**: this is empirical validation in OUR test
> environment. Other environments may have subtly different timing,
> chardev backends, or load patterns. The watchdog + SIGUSR1 dump are
> kept as defense in depth and diagnostic instrumentation for any
> failure mode we haven't yet observed.

### Documentation

- New `docs/TIGER_ON_PVE.md` — definitive setup guide for running Tiger
  10.4 Intel on Proxmox VE / QEMU, including the slirp user-mode-NAT
  workaround for the otherwise-unsolved e1000 PHY auto-neg bug that
  makes tap/bridge networking unusable on Tiger guests under modern
  QEMU. Includes complete `qm config`, OpenCore quirks table, SSH
  cipher recipe, and a troubleshooting matrix covering every symptom
  we hit during the v2.5.4 validation work.

### Late-cycle additions (baked into v2.5.4, no version bump)

After the initial v2.5.4 release commits landed and the agent was
exercised on a fresh Leopard 10.5.8 install, two additional fixes
landed under the same v2.5.4 banner before release tagging:

- **`fix(exec)`** (`7883573`) — bypass i386 libc `execvp()` wrapper for
  absolute paths. Apple's pre-10.6 i386 libc `execvp()` wrapper silently
  returns failure on absolute paths in a way that loses errno; the new
  `exec_child_image()` helper in `src/cmd-exec.c` calls `execv()`
  directly for slash-containing paths (bypassing the wrapper), with a
  `/bin/sh path args` ENOEXEC fallback and errno propagation to
  captured stderr. Discovered when the Leopard i386 slice's `guest-exec`
  silently returned exit 127 for every binary; Codex CLI ran the
  root-cause investigation. Verified after patch: `/usr/bin/true`,
  `/bin/echo`, `/usr/bin/whoami`, `/sbin/ifconfig`, etc. all succeed
  from the i386 slice. No effect on x86_64/arm64 (`execv` is fine on
  those archs too).

- **`log(watchdog)`** (`6490a99`) — drop Tiger-specific wording from the
  idle-channel watchdog message, downgrade `WARN` → `INFO`. The
  v2.4.x-era message text said "Tiger serial driver may be wedged"
  because the watchdog was introduced specifically to defend against
  Tiger's `selrecord` bug. Post-v2.5.4 (after the EOF-storm root-cause
  fix), the watchdog is defense-in-depth only and fires on any 600 s
  idle on any supported macOS — the misleading wording made the same
  benign idle cycle look like a Tiger-specific regression on Leopard
  and El Capitan logs during the v2.5.4 validation sweep. New message:
  *"No message received in 600 seconds — cycling channel
  (defense-in-depth; harmless on idle systems)"*.

- **`docs(COMPATIBILITY)`** (`0822407`) — runtime evidence drop for
  v2.5.4 across 10.4-10.11. Promotes 10.4.11 Tiger, 10.5.8 Leopard,
  and 10.6.8 Snow Leopard to full Tier 1 with current v2.5.4 artifact
  runtime evidence (replacing the prior `1†` current-artifact-retest-
  pending qualifiers). 10.11.6 El Capitan refreshed to note this is
  the fourth consecutive v2.5.x release confirmed structurally
  identical on real Xserve3,1 hardware.

- **`docs(evidence)`** (`ab2cd03`, `880a435`, `17be907`) —
  release-readiness sweep evidence under `docs/evidence/v2.5.4/sweep/`.
  60+ distinct test scenarios per VM, 240+ tests total, 100% pass rate
  across all four PVE guests (BAM-Xserve 10.11, Tiger 10.4.11,
  Leopard 10.5.8, SL 10.6.8). All 8 Codex-prioritized tests pass.

- **`fix(network)`** (`904783c`) — Tiger 10.4 `getifaddrs()` hangs
  indefinitely when called from a long-running daemon. The same
  `getifaddrs()` call from a freshly-spawned SSH-shell process on the
  same Tiger VM returns < 50 ms — the bug is specific to Tiger's libc
  + sysctl/kqueue interaction in the long-running daemon context.
  PVE's default QGA timeout (~5 s) ran out before `getifaddrs()`
  returned, wedging the host-side QGA chardev proxy state. Fix: on
  Tiger 10.4 (Darwin 8.x) return an empty array immediately. The QGA
  spec permits this and PVE's verify.sh + freeze/backup critical path
  do not depend on the command; operators needing network info on
  Tiger can use `qm guest exec /sbin/ifconfig`. Other macOS versions
  (10.5+) get the full interface list as before — verified across all
  four sweep VMs. Same commit also consolidates `netstat -ibn` from
  N forks (per-interface) to 1 fork per response across all macOS
  versions — found during the bisect, real optimization, kept.

- **`fix(network)`** (`6754a36`) — same Tiger-daemon-popen slowness
  applied to `handle_network_get_route()`. `netstat -rn` is fast
  (~50 ms) from a normal shell but takes ~11 s when popen'd from the
  agent daemon on Tiger, exceeding PVE's ~5 s QGA timeout. Unlike
  `getifaddrs()` the call DOES eventually return so the chardev does
  not wedge — but PVE has given up by then. Same Tiger 10.4 (Darwin
  8.x) short-circuit: return an empty array immediately. Other
  macOS versions still get the full routing table.

  Also clears two earlier sweep "open caveats" as false alarms after
  re-testing with corrected harnesses: fsfreeze hook scripts DO
  receive `action="freeze"` / `"thaw"` as `$1` (the prior empty `$1`
  observation was a heredoc quoting bug in the test); and
  `--self-test-json` output IS valid JSON when invoked correctly via
  `qm guest exec` (prior pollution observation was a test invocation
  artifact, not an agent bug).

## v2.5.3 — 2026-05-29

### UX polish — self-source for --install and --upgrade
The binary now knows where it lives. Two ergonomics wins:

- **`mac-guest-agent --install` from anywhere just works.** Previously: operator had to `mv binary /usr/local/bin/ && /usr/local/bin/mac-guest-agent --install` (two steps). Now: `sudo /tmp/mac-guest-agent --install` self-copies from `/tmp/` to `BINARY_PATH` and proceeds. The README's manual install still works as documented (the self-copy is a no-op when binary is already at BINARY_PATH); operators following the bootstrap wrapper or copying via scp benefit.
- **`--upgrade` no longer takes a PATH argument.** Previously: `sudo /tmp/mac-guest-agent --upgrade /tmp/mac-guest-agent` (specify source path explicitly — operator typed the same path twice, which felt dumb). Now: `sudo /tmp/mac-guest-agent --upgrade` — the running binary uses itself as the source. The operator's mental model becomes "run the new binary, tell it to upgrade." `--update PATH` (deprecated) kept for the rare case where an operator genuinely needs to specify a separate source path.

Resolved via `_NSGetExecutablePath` + `realpath()` in `src/service.c::get_self_executable_path()`. Self-copy guards against the degenerate case where source and dest are the same file (refuses with a "run the NEW binary, not the already-installed one" message).

`scripts/install.sh` simplified to match: no longer inserts a path argument after `--upgrade`, no longer pre-copies the binary before `--install` (the binary self-copies). Down another handful of LoC.

### Refactor — install state machine moved into the binary
v2.5.3's first attempt put `--virtio` / `--upgrade` / detection in `scripts/install.sh`. Review during the v2.5.3 cycle exposed the wrong-home argument: the README has documented the binary-direct install path (not install.sh) as primary since v2.4.x, so install.sh's new orchestration features were hidden behind a tool operators following the README wouldn't discover. The kubevirt audience also had to transfer two files instead of one. The refactor moves all orchestration into the binary; install.sh becomes a thin bootstrap wrapper.

**`src/service.c` grows the full state machine:**
- New `service_install(int dry_run, install_mode_t mode)` — mode is `INSTALL_MODE_STANDARD` (the existing flow), `INSTALL_MODE_VIRTIO` (gated override with SIP/macOS/Apple-agent/VirtIO-device prereq checks, interactive yes/no via `/dev/tty`, `launchctl unload -w` Apple's daemon, verify-via-launchctl-list-and-lsof, config write, marker drop, mode-aware functional verify, rollback), or `INSTALL_MODE_VIRTIO_FORCE` (no prereqs, no unload, no prompt — DIY path).
- New `service_upgrade(const char *new_binary_path, int dry_run)` — detects state, backs up current binary to `.backup`, copies new binary, re-runs `--install` to regenerate the plist (the crucial difference from `--update`), restarts, mode-aware verify, rolls back on failure by restoring the backup and re-running its `--install` to regenerate the matching plist.
- `service_uninstall` is now marker-aware: reads `/var/db/mac-guest-agent/.virtio-mode`, removes `/etc/qemu/qemu-ga.conf` only if a marker is present, reloads `AppleQEMUGuestAgent` if mode=full, leaves it alone if mode=force.
- `service_update` becomes a thin deprecation wrapper that delegates to `service_upgrade` (operators using `--update` automatically get the better behavior).
- New `detect_install_state()` / `operator_config_exists()` public API. Test hooks: `MAC_GUEST_AGENT_TEST_STATE` and `MAC_GUEST_AGENT_TEST_CONFIG_EXISTS` env vars let the test suite exercise refusal paths without privileged setup.
- Helpers for SIP-status check (popen `csrutil status` + parse), launchctl-list parse, lsof-equivalent device-holder probe, TTY-confirmation read (`fopen("/dev/tty")`, pipe-resistant), log-tail since byte offset (avoids stale-line race on idempotent re-installs).

**`src/main.c` exposes new flags:**
- `--virtio` and `--virtio-force` as modifiers for `--install`
- `--upgrade PATH` as a peer of `--install` / `--uninstall` / `--update`
- Mutex enforcement (`--virtio` + `--virtio-force` rejected, `--upgrade` + `--install`/`--uninstall`/`--update` rejected, `--virtio[-force]` without `--install` rejected)
- `--help` text expanded to describe all new flags

**`scripts/install.sh` slimmed dramatically:**
- ~770 LoC → ~256 LoC (most of the new size is the embedded `--help` text)
- All orchestration logic removed — install.sh is now a bootstrap wrapper that fetches the binary (or uses `--local`), validates arch, and exec's the binary with the install action forwarded
- `--virtio`, `--virtio-force`, `--upgrade` are forwarded to the binary verbatim
- `--uninstall` exec's the already-installed binary's `--uninstall` directly (no need to download)
- Same operator-facing UX preserved: `sudo bash install.sh --virtio` still works, just delegates to the binary

**`tests/test_install_flags.sh` rewritten:**
- Tests the binary's flag handling directly (refusals, mutex, deprecation, help text) via test-hook env vars
- Small wrapper smoke section confirms install.sh parses + forwards correctly
- 37 assertions, all green

**Docs updated:**
- `README.md` Quick Start mentions `--install --virtio` for kubevirt operators, plus the bootstrap-wrapper one-liner alongside the manual install
- `docs/NO_ISA_OVERRIDE.md` rewritten to use `mac-guest-agent --install --virtio` throughout, with `--virtio-force` documented as the DIY alternative
- `docs/CLI.md` lists `--virtio`, `--virtio-force`, `--upgrade` in the binary's options table

**Net effect:**
- Single-file install for kubevirt operators: `scp binary && sudo mac-guest-agent --install --virtio`
- `--virtio` is discoverable via `mac-guest-agent --help` (no longer hidden in install.sh)
- install.sh's role is now clear: bootstrap (download + copy), nothing else
- All v2.5.3 audit closures stay in place — the refactor reshapes WHERE the logic lives, not WHAT it does

### Audit closures — second pass (2026-05-29)
The first 9 findings (from the earlier 2026-05-29 audit) closed cleanly. A second audit against `36a7425` (post first-round closure) surfaced 5 more findings, all in adjacent surfaces. All 5 closed.

- **MED-1 (binary `--update` was weaker than `install.sh --upgrade`).** `src/service.c::service_update()` did the bare minimum — swap binary + restart — without regenerating the LaunchDaemon plist from the new binary and with weak rollback semantics. Closed by deprecation: `service_update()` now prints a deprecation notice on every invocation pointing at `install.sh --local PATH --upgrade` as the canonical replacement. The binary path stays functional so existing automation doesn't break in this release; removal is queued for a future cycle. `docs/CLI.md` updated to mark `--update` deprecated.
- **MED-2 (`.pkg` postinstall masked failures).** `scripts/build-pkg.sh` generated a postinstall script that ran `--install 2>/dev/null || true; echo "installed"; exit 0`, so package installation reported success even when `--install` failed on unsupported arch, plist-write failure, or `launchctl load` failure. Closed: postinstall now surfaces `--install` errors to stderr, exits non-zero on failure (so `installer` CLI and Installer.app see a failed install), and gates on `uname -m` to fail early on unsupported architectures.
- **MED-3 + the deeper inconsistency it surfaced (libvirt verifier transport).** The audit caught a regex narrowness in `verify.sh`'s libvirt config check — `target type='isa-serial'` only matched type-first attribute order, rejecting valid XML like `<target port='0' type='isa-serial'/>`. But the regex bug was a symptom of a structural inconsistency: the libvirt transport used `virsh qemu-agent-command` exclusively, and libvirt's QGA infrastructure (per `src/qemu/qemu_process.c` in libvirt-the-project) only discovers a guest agent from `<channel type='virtio' name='org.qemu.guest_agent.0'>` elements. Our documented ISA serial setup via `<serial type='unix'><target type='isa-serial'/></serial>` is invisible to libvirt's QGA layer — `virsh qemu-agent-command` returns "QEMU guest agent is not configured" for the configuration we tell operators to use. So the verifier passed XML it then couldn't communicate with.

  Closed by structural rewrite, not regex patch:
  - **`scripts/verify.sh`** libvirt transport now uses **direct unix socket I/O**. Preflight parses `virsh dumpxml` to discover the socket path from the `<serial type='unix'>` element's `<source path='...'/>` (tolerating any attribute order — XML 1.0 and Relax NG don't constrain attribute order, and external tooling / `virsh edit` can produce non-canonical order that libvirt still accepts). The transport then delegates to the existing `_qga_socket_cmd` / `_qga_socket_guest_exec_json` helpers — the same machinery the UTM and qga-socket transports use. `virsh qemu-agent-command` is never called by the new libvirt path.
  - **`docs/LIBVIRT.md`** rewritten. Explicit explanation: libvirt's QGA infrastructure is VirtIO-only, so `virsh qemu-agent-command` does not work for our ISA configuration. The "Guest Agent Commands via virsh" section is replaced with "Guest Agent Commands — Direct Socket Access" using `socat`/Python/Perl examples. The "Snapshots with Quiesced Freeze" section is replaced with an honest note that `virsh snapshot-create-as --quiesce` does NOT call our freeze handler (libvirt's QGA layer can't see us), and shows the manual three-step freeze → snapshot → thaw workflow operators should use. Operators who specifically need libvirt's native QGA API working can use the `--virtio` install path (see `docs/NO_ISA_OVERRIDE.md`).
  - **`tests/test_verify_transports.sh`** section 5b rewritten. Spawns a perl QGA listener bound to a real unix socket, has the virsh shim return XML pointing at that socket, asserts that verify.sh discovers the path and talks to it end-to-end. Three test cases: documented attribute order (pass), reordered attribute order (pass — defense for the audit's MED-3 regex narrowness), VirtIO-channel-only XML (fail — preflight refuses with a "could not discover QGA unix socket" message). The shim now explicitly fails loud if anything tries to call `virsh qemu-agent-command`, so a future regression that reintroduces the wrong path triggers a test failure rather than going undetected.
  - Net result: the libvirt verifier transport works end-to-end against the configuration we document for the first time since v2.5.0. The "Channel prereq" inconsistency between docs and verifier (silent since v2.5.0, made worse by the first-round MED-3 fix that changed the XML check to ISA serial without updating the transport) is closed.
- **LOW-1 (`--virtio` operator-config refusal pointed at a broken remediation).** The previous message offered "hand-edit the config + run the standard install" as an alternative remediation. That path produced a setup with our config pointing at VirtIO while Apple's daemon still owned the channel, and no marker file for `--uninstall` to do the right thing. Rewritten to recommend backup-then-`--virtio` as the managed path, with `--virtio-force` documented as the explicit DIY alternative for operators who've already arranged for Apple's daemon to not own the channel.
- **LOW-2 (stale test counts in docs).** `CHANGELOG.md`'s reference to "34-assertion test suite" and `docs/COMPATIBILITY.md`'s "48 unit + 31 proactive + …" counts were stale. Replaced with version-agnostic phrasing ("current counts in `make test` output") that won't go stale every release cycle.

### Added — install/upgrade state machine
- **`scripts/install.sh` now detects existing install state and routes accordingly.** Before v2.5.3 install.sh was both "install" and "re-install/upgrade" with no distinction — re-running it just did stop / copy / regen plist / restart, treating fresh installs and upgrades identically. v2.5.3 adds explicit state-aware routing while preserving the historical "just re-run install.sh" workflow.

  **New detection layer:** `detect_install_state()` returns one of `not-installed`, `standard`, `virtio-full`, `virtio-force` (read from the `/var/db/mac-guest-agent/.virtio-mode` marker for `virtio-*`, or from binary/plist presence for `standard`). `operator_config_exists()` checks `/etc/qemu/qemu-ga.conf` independently. Both have documented test hooks (`MAC_GUEST_AGENT_TEST_STATE`, `MAC_GUEST_AGENT_TEST_CONFIG_EXISTS`) so the hermetic test suite can exercise every state without privileged setup.

  **New `--upgrade` flag (explicit upgrade in place).** Refuses if no install is detected. Preserves the detected install mode (standard / virtio-full / virtio-force). Swaps the binary into place, backs up the old one to `/usr/local/bin/mac-guest-agent.backup`, regenerates the LaunchDaemon plist via the binary's `--install`, restarts the daemon, and verifies functional state per mode (VirtIO modes check the agent log shows `Opened device: $VIRTIO_DEVICE` past a captured pre-restart byte offset; standard mode checks the daemon has a running PID). On verify failure: restore the backed-up binary, re-run its `--install` to regenerate its matching plist, exit non-zero. `/etc/qemu/qemu-ga.conf` is NEVER touched in any upgrade path — operator state (allow-rpcs / block-rpcs / verbose / pidfile / logfile / etc.) survives by construction.

  **Bare `install.sh` invocations auto-route to the upgrade path when an existing install is detected.** Preserves the v2.4.x → v2.5.x re-run workflow. A one-line `[INFO]` notice surfaces the auto-routing so the behavior is transparent.

  **`--virtio` / `--virtio-force` are now fresh-install-only.** Refuses with a specific message + remediation pointer (`--upgrade` to update, `--uninstall` first to switch modes) when an existing install is detected. Also refuses if `/etc/qemu/qemu-ga.conf` exists without our marker (operator state present that the override would have clobbered).

  **`--upgrade` is incompatible with `--virtio` / `--virtio-force`.** Upgrades preserve the detected mode; passing both is ambiguous.

  **Why this design:** the alternative was a "merge into `/etc/qemu/qemu-ga.conf` with marker comments" approach. Discussion exposed that the merge approach (and a separate "PlistBuddy injection into the LaunchDaemon plist's ProgramArguments" alternative) had update-lifecycle hazards — the PlistBuddy variant would silently wipe the override on every `install.sh` re-run because `service.c` regenerates the plist from embedded data. Detection-driven refusal closes the same operator-data-loss vector as the merge approach (HIGH-1 from the 2026-05-29 audit) by elimination rather than by careful merge logic, and preserves the lifecycle that's worked across v2.4.x → v2.5.2.

### Audit closures (2026-05-29)
- **HIGH-1 — `--virtio` would have clobbered operator state in `/etc/qemu/qemu-ga.conf`.** Closed structurally by the detection-driven refusal above. `--virtio` and `--virtio-force` now refuse on any pre-existing operator config, with a specific remediation message.
- **MED-1 — `--virtio` rollback left the binary and LaunchDaemon plist in place after a failed functional verify.** Fixed: rollback now calls the binary's own `--uninstall` (if present) or manually unloads + removes the plist, then `rm`s the installed binary, before restoring Apple's daemon. After rollback no dormant agent state remains that could relaunch on next manual `launchctl load`.
- **MED-2 — Unknown flags / extra args silently consumed.** Typos like `install.sh --virto` fell through to the default install path. Fixed: after `--local PATH` resolution, any remaining positional argument produces `Unknown argument: $1` and exits non-zero. Three new test cases in `tests/test_install_flags.sh`.
- **MED-3 — `scripts/verify.sh libvirt_config_summary()` flagged correctly-configured VMs as misconfigured.** The function checked for `org.qemu.guest_agent.0` (VirtIO channel name) — exactly the opposite of the project's documented ISA-only requirement. Operators following `docs/LIBVIRT.md` would see their valid configuration marked FAIL and be told to add a channel the rest of the project warns against. Fixed: now greps for `target type='isa-serial'` / `target type="isa-serial"` instead. Failure message points at `docs/LIBVIRT.md`. Same-class fix in `docs/LIBVIRT.md` — the "Channel prereq" paragraph in the verifier section now describes the ISA serial element (was contradicting the early-section example).
- **MED-4 — CI did not run the new install-flag tests.** `make test` wired `test-install-flags` but `.github/workflows/build.yml` and `release.yml` invoked individual targets that omitted it, so the parser surface was local-only-tested. Fixed: both workflows now run `make test-install-flags`. `release.yml` also gained `make test-verify-transports` for symmetry.
- **MED-5 — `tests/test_verify_transports.sh` claimed libvirt coverage but had no `virsh` shim block.** This is why MED-3 survived undetected. Fixed: new section "5b. libvirt transport (virsh shim) — config check covers MED-3" with 6 assertions. Pass case (ISA serial XML) and fail case (the VirtIO-channel XML the legacy verifier was wrongly happy with). Asserts the right verdict, the right failure-text contents, and that the verifier no longer references the VirtIO channel name on the pass path.
- **LOW-1 — `docs/NO_ISA_OVERRIDE.md` overstated the `--virtio` install's functional verify** ("confirms the agent answers guest-info"). The code checks PID + log line "`Opened device:`", not a host-to-guest QGA round-trip. Fixed: doc wording now matches behavior exactly, with an explicit note that the check is local-state-only.
- **LOW-2 — `docs/TESTING_HARNESS.md` hardcoded v2.5.1 as the latest release** in three places. Fixed: now version-agnostic ("the release you downloaded", "current `main` release — check the GitHub releases page").
- **LOW-3 — `configs/pve/*.conf` told users to do thin per-arch builds** (`make build-x86_64`, `make build-i386`) which no longer match the v2.5.0+ universal-only release model. Fixed: comments now point at the universal `mac-guest-agent` artifact and note which slice dyld picks per host class.

### Added — opt-in unsupported feature
- **`scripts/install.sh --virtio` and `--virtio-force`: gated VirtIO-transport install path for orchestrators that hardcode VirtIO at the libvirt-channel level (kubevirt today; any orchestrator with the same constraint qualifies).** Full operator-facing contract in `docs/NO_ISA_OVERRIDE.md`. ISA serial remains the only supported transport — this is an opt-in escape hatch for the small population (macOS 11+, SIP disabled, orchestrator forces VirtIO) where ISA is not an option on the host. Auto-detect behavior unchanged; no transport-mode flips; no philosophy change.

  **`--virtio` (documented, safety-gated)**:
  - Prerequisite checks: macOS >= 11, SIP disabled (`csrutil status`), `AppleQEMUGuestAgent` LaunchDaemon present at `/System/Library/LaunchDaemons/`, VirtIO guest-agent device present at `/dev/cu.org.qemu.guest_agent.0`. Every check refuses with a specific actionable message; no install actions run if any check fails.
  - Interactive warning block + `yes/no` prompt read from `/dev/tty` (NOT stdin) — so `yes | install.sh --virtio` cannot bypass the gate.
  - On confirmation: `launchctl unload -w` Apple's LaunchDaemon, then **verify the unload actually landed** via two probes: (a) `launchctl list` no longer shows the daemon label, (b) `lsof` on the VirtIO device shows no holder. If either probe fails, abort and attempt to reload Apple's daemon to restore the prior state.
  - Standard agent install + write `/etc/qemu/qemu-ga.conf` with `path = /dev/cu.org.qemu.guest_agent.0` + drop marker at `/var/db/mac-guest-agent/.virtio-mode` (content: `mode=full`).
  - Functional verification: agent process PID is non-`-` in `launchctl list`, and `/var/log/mac-guest-agent.log` shows `Opened device: /dev/cu.org.qemu.guest_agent.0` within 5 seconds. If either fails, roll back (remove our agent, remove config, remove marker, reload Apple's daemon).

  **`--virtio-force` (undocumented; visible only in `--help`)**:
  - Bypasses every prerequisite check. No SIP probe. No Apple-agent unload. No `/dev/tty` prompt.
  - Installs the agent + writes the override config + drops marker with `mode=force`.
  - For experts who have already configured the host manually (Apple's daemon unloaded by hand, SIP off by hand, non-standard device path, etc.) and want a one-line install without re-running the same checks the gated path imposes.
  - Not in `README.md`, not in `docs/NO_ISA_OVERRIDE.md`. Hostile-named on purpose.

  **`scripts/install.sh --uninstall`** (new): detects the marker, removes the override config, and:
  - `mode=full` (gated install): reloads Apple's `AppleQEMUGuestAgent` LaunchDaemon to restore prior state.
  - `mode=force` (force install): does not touch Apple's daemon (we didn't unload it; the operator did).
  - SIP is NOT re-enabled by `--uninstall` in either mode — that's an operator action via Recovery + `csrutil enable`.

  **Argument-parsing rejections** (hard errors before any side effect): `--virtio` + `--virtio-force` cannot combine; `--uninstall` cannot combine with `--virtio` / `--virtio-force` / `--dry-run`.

  **`--dry-run` support**: `--dry-run --virtio` and `--dry-run --virtio-force` print the would-do plan for the override paths (including the Apple-unload + verify steps for `--virtio`, and the no-checks notice for `--virtio-force`), no side effects, no root required. Same UX as the existing standard-install dry-run.

- **`docs/NO_ISA_OVERRIDE.md`**: full operator contract for `--virtio`. macOS 11+ scope stated up front; SIP-off rationale explained structurally (Apple's LaunchDaemon plist lives in `/System/Library/`, every Apple-supported override path is SIP-protected, no engineering workaround on our side); risk block; rollback instructions; explicit statement that this configuration is NOT covered by release-to-release stability promises; explicit non-decision on shipping a DriverKit System Extension to avoid the SIP-off requirement (months of engineering + paid Developer Program + notarization + user approval + ongoing IOKit-match arbitration against Apple per release, for a population this page already describes as small — explicitly not the right ROI for the project).

- **`tests/test_install_flags.sh`**: a test suite covering argument parsing, mutually-exclusive flag rejection, `--help` mentions of new flags, and dry-run plan output for `--virtio` / `--virtio-force` / `--upgrade` / default. Expanded across the v2.5.3 audit cycles to also cover detection-state transitions and unknown-arg rejection. Wired into `make test` via the new `test-install-flags` target. Does not exercise live `csrutil` / `lsof` / `launchctl` probes (those need PATH-stubbed system commands and are not worth the test-infrastructure cost for an unsupported feature) — manual verification on the El Cap and a Big Sur+ VM covers the live-probe paths.

### Changed
- **`src/channel.c log_virtio_diagnostic_if_present()`**: the "no ISA device found, but a VirtIO device is present" diagnostic message now closes with a one-sentence pointer at `docs/NO_ISA_OVERRIDE.md` for operators whose orchestrator hardcodes VirtIO and cannot expose ISA. Same diagnostic surface, no change in detection logic. Surfaces the escape hatch to anyone hitting the error without re-adding any auto-detect path.

### Not changed
- **ISA serial remains the only supported transport.** Auto-detect (`src/channel.c known_devices[]`) is unchanged — `--virtio` does not modify it. Default installs are not affected by anything in this release.
- **No `transport = virtio` config key, no first-class VirtIO transport, no auto-detect of VirtIO devices, no DriverKit System Extension, no kext.** Every option-space entry that would erode the v2.5.0 ISA-only decision was considered and rejected — see commit messages and the discussion behind issue #7 (mav2287/mac-guest-agent) for the rationale.

## v2.5.2 — 2026-05-28

### Fixed
- **`guest-exec` process-table slot leak (DoS after 64 short execs even with correct status polling).** `src/cmd-exec.c::process_table` caps at `MAX_PROCESSES = 64`. Slots were reclaimed only by a 30-minute wall-time cleanup, so a caller polling `guest-exec-status` until `exited:true` (the correct usage pattern) would still see the slot held for half an hour. After 64 short execs the 65th returned `GenericError: Too many running processes` until the cleanup window passed — affecting backup tools, monitoring loops, and the project's own `scripts/verify.sh` which uses `guest-exec` for the in-VM `--self-test-json` and `--safe-test-json` calls. Reproducer (now a regression test in `tests/run_tests.sh`): launched `/bin/echo` + poll-until-exited 64 times → 65th failed.

  Fix: `handle_exec_status()` now calls `release_process(proc)` after the terminal `exited:true` response is fully built (the response JSON owns its own malloc'd b64 strings, so freeing `proc->out_buf` / `proc->err_buf` is safe). The 30-minute cleanup in `alloc_process()` stays as the safety net for callers who launched and never polled. New regression test runs 100 short execs with `poll-until-exited` and asserts every single one succeeds; sabotage-verified the test catches the bug at iteration 64 without the fix.

  **Behavioral change**: a `guest-exec-status` call against a PID that already received its terminal status now returns `InvalidParameter`. The QGA spec does not guarantee idempotent terminal polling and we never documented it; the common pattern is "poll until exited, then move on."

### Documentation
- **`docs/TESTING_HARNESS.md` unblocked for contributors.** The Profile B verify.sh download URL pointed at the deleted `universal-upgrade-v2.4.4` branch (404'd as soon as PR #6 merged). Updated to `main`. Stale `agent_version = 2.5.0` expectations replaced with "the version you installed." Same applies to evidence-expectation wording further down.
- **`docs/COMPATIBILITY.md` introduces Tier 1†** (Production-ready, current-artifact retest pending) for 10.4 Tiger and 10.5 Leopard. Runtime evidence on those rows is still v2.4.3 (vit9696's PR #5); the i386 slice's build recipe is unchanged from v2.4.3 → v2.5.x and `scripts/verify-legacy-slices.sh` confirms structural equivalence on every CI build, so the rows remain Tier 1 in spirit but the dagger flags the pending current-release runtime drop. Promotes back to plain Tier 1 once a v2.5.x evidence drop lands.
- **Cleanup sweep**: tracked source / scripts / workflows no longer reference the deleted `audit.md` and `universal_upgrade.md` files. `docs/design/AGENT_BEHAVIOUR_SPEC.md` and `docs/research/UPSTREAM_NOTES.md` now marked as "historical reference" rather than "in progress" — the work they describe shipped in v2.4.3.

## v2.5.1 — 2026-05-28

### ⚠️ BREAKING CHANGE — release asset filename
The published release asset is renamed from `mac-guest-agent-darwin-universal` (v2.5.0) to simply `mac-guest-agent` (v2.5.1+). Same tri-fat universal binary, shorter name. The shorter name matches what `/usr/local/bin/mac-guest-agent` will contain post-install — the manual install flow becomes:

```bash
curl -fLO https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent
sudo mv mac-guest-agent /usr/local/bin/
sudo /usr/local/bin/mac-guest-agent --install
```

instead of requiring a per-step rename. Requested by @vit9696 ([#4](https://github.com/mav2287/mac-guest-agent/issues/4)).

**Anyone with v2.5.0 URLs** (one-day window between v2.5.0 and v2.5.1) must update:

| Was (v2.5.0) | Now (v2.5.1+) |
|---|---|
| `…/releases/latest/download/mac-guest-agent-darwin-universal` | `…/releases/latest/download/mac-guest-agent` |

`scripts/install.sh --local` accepts both names during the transition — the v2.5.0 `*-darwin-universal` filename is kept in the search list as a recovery fallback so users who downloaded yesterday's release don't need to rename. `service.c --update` text and all install snippets updated to the new name.

### Fixed
- **`producer | short-circuit-consumer` SIGPIPE race class eliminated across all `set -o pipefail` shell scripts.** Under pipefail, a pipeline whose right-hand side exits early (`grep -q`, `head -1`, `awk '...{print; exit}'`, `sed 'Nq'`, etc.) SIGPIPEs the producer's next write, which dies with status 141 and propagates non-zero through the pipeline — making the surrounding `if` or `$()` see a "failure" that didn't logically happen. Hit once in the wild as the "PVE: VMID redacted in human output" flake on CI run 26532052157 (commit `d0bde24`, macos-14, 2026-05-27); other instances of the same pattern existed in the codebase and would have been timing-bombs.

  Sites fixed:
  - `tests/test_verify_transports.sh` `assert_contains` / `assert_not_contains` rewritten to use bash `case "$haystack" in *"$needle"*)` pattern matching — pure bash, no subprocess.
  - `scripts/verify-legacy-slices.sh` `slice_min_macosx` / `slice_build_version_minos` awk extracts: removed `; exit` from the awk action and added `flag=0` to clear after first match. awk reads to EOF; otool finishes writing cleanly.
  - `scripts/verify-legacy-slices.sh` gate 3h (host_statistics64 weak-import check): rewritten in pure bash with a `while IFS= read … <<<"$nm_full"` loop plus `case` matching.
  - `tests/run_tests.sh` all 11 `awk 'NR==1{...; print; exit}'` invocations: `; exit` removed (awk reads to EOF).
  - `tests/run_tests.sh` two `| sed 's/^QMP> //' | head -1` chains: collapsed to `| awk 'NR==1{sub(/^QMP> /,""); print}'` (single awk that reads to EOF).

  Each affected file also gained a banner comment near its `set -o pipefail` line documenting the convention (no `producer | short-circuit-consumer` under pipefail) with a reference to this CI incident, so future contributors don't re-introduce the pattern. Verified locally with 5x consecutive runs of `make test`, `./scripts/verify-legacy-slices.sh`, `./tests/run_tests.sh`, and `./tests/test_legacy_slice_gate.sh` — every run clean.

### Added
- **`--dry-run` flag for `--install` / `--uninstall` / `--update`.** Plumbed through `src/service.c` so the three handlers gate every side-effect (filesystem writes, file copies, `unlink`, `rename`, `launchctl` calls) on the flag and print "DRY RUN: would ..." lines instead. Root check is also skipped in dry-run because no privileged operations execute. Non-destructive validation (binary path existence, executable bit) still runs — so a `--update /no/such/file --dry-run` invocation fails fast with the right error, exactly as the real `--update` would. Pairs with `scripts/install.sh --dry-run` (added in v2.5.0): the script side covers download / path resolution / cp + chmod planning, the binary side covers the LaunchDaemon plist write, log rotation config, and launchctl load/start. Together they give end-to-end smoke-testability of the install flow without root or a clean VM. Help text + manpage + `docs/CLI.md` updated.

### Removed
- **`cfg.method` config field and `-m` / `--method` CLI flag.** The field was already vestigial in v2.5.0 — VirtIO transport was removed, leaving `auto` and `isa-serial` as functionally identical synonyms with no behavior to gate (channel selection in `src/channel.c known_devices[]` is ISA-only regardless). v2.5.1 removes the field from `struct config`, removes `DEFAULT_METHOD`, drops the `-m`/`--method` flag (getopt returns "unknown option"), drops the `method =` line from `--dump-conf` output, and updates help text + `configs/qemu-ga.conf` accordingly. Use `-p PATH` / `path = /dev/cu.serial1` (which already exists) for explicit device-path override.

  **Migration:** existing `/etc/qemu/qemu-ga.conf` files that still contain `method = auto`, `method = isa-serial`, or `method = virtio-serial` lines will continue to parse — the parser accepts the key and emits a one-time notice on stderr ("the `method` config key was removed in v2.5.1 and is ignored …") pointing the user at removing the line. No exit, no error. The v2.5.0 hard-rejection of `method = virtio-serial` softens to the same deprecation notice because the field no longer has any behavior to misconfigure.

  **Why now:** the ISA-only transport decision in v2.5.0 collapsed the field's value space from three distinguishable options (`auto` / `isa-serial` / `virtio-serial`) to one (any value → ignored), making the surface honest about there being no choice. Keeping the field cost ~15 lines of code spread across `src/main.c` plus a doc paragraph explaining why it existed but did nothing.

## v2.5.0 — 2026-05-27

### ⚠️ BREAKING CHANGE — release asset filename
The release ships a **single** binary: `mac-guest-agent-darwin-universal` (i386 + x86_64 + arm64 in one tri-fat Mach-O; dyld picks the right slice at load time). The previous per-architecture assets are gone:

- `mac-guest-agent-darwin-amd64` → **removed**
- `mac-guest-agent-darwin-arm64` → **removed**
- `mac-guest-agent-darwin-i386` → **removed** (was Makefile-only, never officially published)

Anything pinning the old URL — install scripts, Ansible/Salt/Chef recipes, CI jobs, IaC, package manifests — must update to:

```
https://github.com/mav2287/mac-guest-agent/releases/latest/download/mac-guest-agent-darwin-universal
```

One download URL now covers macOS 10.4 Tiger through 26 Tahoe. The version bump to **2.5.0** (rather than a 2.4.4 patch) reflects that this is a backward-incompatible release-shape change, not a drop-in patch.

### ⚠️ BREAKING CHANGE — ISA serial transport only
The agent now supports **ISA serial only**; the VirtIO transport fallback that v2.4.x carried in `src/channel.c known_devices[]` has been removed. The new contract:

- `known_devices[]` is ISA-only (`/dev/cu.serial1`, `/dev/cu.serial2`, `/dev/cu.serial` and their `/dev/tty.*` counterparts).
- A VirtIO-only VM presents no usable channel — the agent logs a clear error message (`"Found VirtIO serial device (...) but VirtIO transport was removed in v2.5.0 — this agent now requires ISA serial. Reconfigure your hypervisor..."`) and exits.
- `method = virtio-serial` is rejected at config-parse time with the same explanation; `method = auto` (default) and `method = isa-serial` continue to work.
- CLI: `-m virtio-serial` is rejected the same way.

Why: VirtIO was always a footgun on Apple Virtualization.framework hosts (UTM Virtualize mode, `vz_run`, anything `VZVirtualMachine`-backed) where Apple's own 18-command `AppleQEMUGuestAgent` claims the channel and silently intercepts traffic; v2.4.x kept it as a fallback for the narrow case of "plain QEMU configs without ISA UART," which produced the surprising behavior that the same install behaved differently depending on host class. Restricting to ISA closes that ambiguity and makes a disk image moving between QEMU and VZ-backed hosts keep working without reinstall.

**Migration from v2.4.x:**
- **PVE:** `qm set <vmid> --agent enabled=1,type=isa` (already the documented setup).
- **libvirt:** add an `isa-serial` device to the domain XML; remove any `virtio-serial` agent channel.
- **UTM:** in VM settings, Devices → Serial → set Interface to **QemuGuestAgent** (ISA-backed). Remove any VirtIO Serial Interface.
- **Raw QEMU:** add `-device isa-serial` to the command line; remove `-device virtio-serial-pci` if it was the agent channel.
- **UTM Virtualize backend on Apple Silicon:** no ISA option exists. Switch the VM to UTM's Emulate (QEMU) backend, or accept Apple's built-in 18-command agent (no freeze) on the VirtIO channel.

After reconfiguring the hypervisor, fully stop and restart the VM (QEMU device changes need a full restart, not a guest reboot).

### Bug Fixes
- **Fixed (compatibility):** `mac-guest-agent-darwin-amd64` v2.4.3 crashed at startup on Mac OS X 10.6 Snow Leopard and 10.7 Lion with `dyld: unknown required load command 0x80000028` (SIGTRAP). The amd64 binary advertised `LC_VERSION_MIN_MACOSX 10.6` but its entry-point load command was `LC_MAIN` (introduced 10.8). The v2.4.3 release pipeline was running on a GitHub Actions runner image carrying Xcode 15.5, which silently clamped the Makefile's `MACOSX_DEPLOYMENT_TARGET=10.6` env var and emitted `LC_MAIN` regardless. Reported by @vit9696 in #4. Fixed by building both legacy slices (i386 + x86_64) against the phracker `MacOSX10.13.sdk` with explicit `-mmacosx-version-min` flags (10.4 for i386, 10.6 for x86_64) AND `-Wl,-ld_classic` to invoke Apple's older linker (Xcode 15-16's new `ld-prime` hardcodes `LC_MAIN` for x86_64 regardless of the min flag; `ld-classic` honors the min flag for entry-point selection). The combination emits `LC_UNIXTHREAD` which 10.6/10.7 dyld understands. Also added `scripts/verify-legacy-slices.sh` invoked by both build and release CI workflows, which fails the build on any disallowed load command, off-spec deployment target, unexpected dylib dependency, weak-import attribute regression on `host_statistics64`, or undefined-symbol drift outside the checked-in per-slice baselines. The gate makes the invariant explicit in CI; current implementation depends on `macos-14` runner + `ld-classic` (deprecated) and will need revisiting if Apple removes `ld-classic` or GitHub retires the runner image (canary build on `macos-latest` watches for both).

### Tooling / Packaging
- **Removed:** VirtIO entries from `src/channel.c known_devices[]`. The auto-detect list went from 14 entries (6 ISA + 8 VirtIO across the various `/dev/cu.virtio*`, `/dev/cu.org.qemu.guest_agent.0`, `/dev/cu.qemu-guest-agent` aliases UTM/QEMU/libvirt expose) down to 6 (ISA only). Added a separate `log_virtio_diagnostic_if_present()` that scans for the removed VirtIO paths only when ISA detect fails, so an upgrading user with a leftover VirtIO setup gets an explanatory error pointing at the migration steps rather than a generic "no serial device found."
- **Removed:** `method = virtio-serial` (config file) and `-m virtio-serial` (CLI) are now rejected at parse time with a message pointing at the v2.5.0 BREAKING entry. `auto` (default) and `isa-serial` continue to work.
- **Changed (release):** v2.5.0 publishes a **single binary**: `mac-guest-agent-darwin-universal`, a tri-fat Mach-O containing `i386 + x86_64 + arm64` slices. dyld picks the appropriate slice at load time: Tiger and Leopard pick i386 (those OSes lack x86_64 user-space support, or in 10.5's case prefer i386); Snow Leopard picks x86_64 when booted with a 64-bit kernel (Xserve / Mac Pro default) or i386 when booted with the 32-bit kernel default on most consumer hardware; Lion through Catalina pick x86_64 (with the `LC_UNIXTHREAD` fix above); Big Sur and Apple Silicon pick arm64. **The thin per-arch binaries (`-i386`, `-amd64`, `-arm64`) are no longer published.** One download URL covers all supported macOS versions and architectures. If the universal doesn't start on a specific host, open an issue at https://github.com/mav2287/mac-guest-agent/issues/new — we work each report as a bug.
- **Changed:** Install URL changed from `mac-guest-agent-darwin-amd64` to `mac-guest-agent-darwin-universal`. Scripts pinning the old URL must update.
- **Added:** `scripts/verify-legacy-slices.sh` — CI-callable script that audits per-slice invariants (LC commands, deployment targets, dylib deps, undefined symbols) of the produced universal. Replaces the previous inline `clock_gettime` check; now runs against all three slices and covers more failure modes. Hard-fails the build on any disallowed `LC_REQ_DYLD` command, unknown numeric load command, missing per-slice symbol baseline, or symbol drift outside `tests/legacy_slice_symbols_<arch>.txt`.
- **Added:** New `surrogate-32bit` CI job builds the portable subset (`protocol.c` + `cJSON.c`) under `gcc -m32` on `ubuntu-latest` via a standalone `tests/surrogate_32bit_main.c` driver and runs portable unit tests under 32-bit code. `selftest.c` is excluded because it drags macOS-specific dependencies (`compat_*`, `run_command_capture`); `log.c` is excluded because it loads `os_log` via `dlfcn` (a macOS-runtime feature with no Linux glibc equivalent); `util.c` is excluded because it `#include`s `compat.h` and uses POSIX surface that needs `_POSIX_C_SOURCE=200809L` on Linux glibc. Catches int-width / struct-layout / endianness regressions in JSON marshaling without depending on access to old Intel Mac hardware.
- **Added:** `#include <stdint.h>` to `src/util.c` (one line, no behavior change) — `SIZE_MAX` was previously visible only via transitive Apple SDK includes; explicit include eliminates that fragility.
- **Changed:** `Makefile` `build-x86_64` now uses explicit `-mmacosx-version-min=10.6 -isysroot $(LEGACY_SDK)` instead of relying on `MACOSX_DEPLOYMENT_TARGET=10.6` env var (which is toolchain-version-dependent and was the underlying mechanism of the v2.4.3 bug). `build-i386` similarly gets explicit `-mmacosx-version-min=10.4`. `LEGACY_SDK` defaults to `/tmp/MacOSX10.13.sdk` (phracker tarball, SHA256 `1d2984ac…23a5a` pinned in CI); `I386_SDK` aliases it for backward compatibility.
- **Changed:** `Makefile` `build-universal` now produces a **tri-fat** binary (i386 + x86_64 + arm64; previously x86_64 + arm64 only).
- **Changed:** `Makefile` `dist` / `pkg` / `sign` / `dsym` / `help` targets all updated for universal-only distribution. `dist` clears `$(DIST_DIR)` before populating so stale per-arch artifacts from previous builds can't leak into the checksums.
- **Changed:** `src/service.c` `--update` flag's instruction text now references `mac-guest-agent-darwin-universal`.
- **Changed:** `scripts/install.sh` fetches the universal binary; `detect_arch()` removed (no per-arch asset to pick) but architecture validation preserved as `validate_arch()` so unsupported hosts (e.g., PowerPC) fail early with a clear message.
- **Changed:** `scripts/install.sh --local` now finds the published release asset by its real filename. The pre-v2.5.0 search list only checked `build/mac-guest-agent`, `./mac-guest-agent`, `/tmp/mac-guest-agent-x86_64`, `/tmp/mac-guest-agent` — none of which match what `service.c --update` or the install docs tell users to download (`mac-guest-agent-darwin-universal`). The script now searches `./mac-guest-agent-darwin-universal` and `/tmp/mac-guest-agent-darwin-universal` first, then `build/mac-guest-agent-universal`, then the legacy generic names for recovery flows. Added explicit `--local /path/to/binary` form so the installer never has to guess: `sudo ./install.sh --local /Users/me/Downloads/mac-guest-agent-darwin-universal`. The error message on "no binary found" now lists every searched path and points at the explicit-path form. `--help` updated.
- **Updated:** Workflow comments in `.github/workflows/build.yml` and `.github/workflows/release.yml` rewritten to name `-Wl,-ld_classic` as the load-bearing dependency (the ld-classic linker is what honors `-mmacosx-version-min` and emits `LC_UNIXTHREAD` on the legacy slices). Previous wording framed the `macos-14` runner pin as the primary mechanism; the pin is just toolchain stability — without `ld_classic` the slices break on any Xcode 15+ regardless of runner image. Documented fallback chain for when Apple removes `ld_classic`: verify canary status, try Homebrew cctools `ld`, or build legacy slices in a container with a frozen older Xcode CLT.
- **Changed:** `scripts/build-pkg.sh` default arch is now `universal`; per-arch invocation kept for internal testing.
- **Changed:** `scripts/verify-installer.sh` recommendation collapsed from per-arch (if/elif/elif on macOS version) to single universal-binary line.
- **Added:** `--self-test-json` `system_info` block now includes a `selected_arch` field reporting which slice of the universal binary dyld actually picked. Useful for verify.sh evidence drops and post-incident forensics.

### Documentation
- **Updated:** `README.md`, `docs/PVE.md`, `docs/UTM.md`, `docs/COMPATIBILITY.md`, `docs/RELEASE_TEMPLATE.md` install snippets all reference the universal binary as the single download. README has a new "If the agent doesn't start" section that asks users to open a GitHub issue with diagnostic outputs (loader-safe `sw_vers` / `file` / `lipo -info` first; `--self-test-json` / `--version` / log tail only if the binary actually starts). Modern-machine TLS caveat preserved — Tiger / Leopard / older Snow Leopard guests usually need to download on a modern machine and transfer the file.

## Unreleased

### Highlights

- **Unified host-side verifier (`scripts/verify.sh`).** Replaces the PVE-only `scripts/pve-verify.sh` with a single auto-detecting verifier that covers Proxmox VE, libvirt, UTM, and any raw-QEMU host with a QGA Unix socket. Each transport reaches host-driven QGA commands and `guest-exec` polling through the same five-primitive plugin interface, so the check pipeline (Configuration → VM State → Agent Communication → Memory → Host Environment → Multi-cycle Freeze/Thaw → In-VM Diagnostics) is identical regardless of hypervisor. PVE auto-detected via `qm` + `/etc/pve/qemu-server/<id>.conf`; libvirt via `virsh dominfo`; UTM via `utmctl status` (with QGA-serial socket discovered from the `.utm` bundle plist); raw QEMU via `--qga-socket PATH`. Per-transport preflights: PVE (root, cluster locality, backup-lock); libvirt (libvirtd reachability); UTM (refuses root, requires QGA serial configured in UTM GUI); qga-socket (path is a real Unix socket). Auto-thaw safety trap on EXIT/INT/TERM. `freeze_dispatch` static-contract check against the agent binary catches drift between `fs_dispatch_class()` and `docs/design/FREEZE_SEMANTICS.md`. Multi-cycle freeze (default 3) catches state-leak bugs the prior single-cycle check missed. Mount-dispatch cross-check compares the frozen count to the captured mount table. JSON appendix schema bumped to 2.0 with `host_environment` (`sw_vers` / hardware / kexts / ioreg-serial-nodes / parsed mount table / launchd / log-file stat), `freeze_cycles_log` (per-cycle structured records), `mount_dispatch_crosscheck`. PII (IPv4 / MAC / supplied identifier) redacted by default. 57-assertion shell-shim test suite (`make test-verify-transports`) covers all four transports without requiring a real hypervisor. Docs swept: `COMPATIBILITY.md` Step 2, `UTM.md`, `LIBVIRT.md`, `PVE.md`, `evidence/README.md` (with full schema 2.0 field table). `pve-verify.sh` deleted — no shim. Tracked as Phase 4 in `docs/PLAN.md`.

### Tooling
- **Added:** `docs/mac-guest-agent.8` is now generated from `docs/mac-guest-agent.8.in` at build time, with `@VERSION@` substituted from the Makefile and `@DATE@` from the build's month/year. New Makefile rule (`docs/mac-guest-agent.8: docs/mac-guest-agent.8.in Makefile`) regenerates whenever either input changes. The `build` target depends on it so a plain `make build` also keeps the manpage fresh. New CI step in `.github/workflows/build.yml` runs the regeneration and `git diff --quiet`s the result — fails CI with a clear "manpage is stale" error if a `VERSION` bump landed without regenerating. Prevents the audit-finding-4-style drift (manpage at 2.2.0 while Makefile at 2.4.2) from ever recurring. The generated `.8` is still tracked in git so `raw.githubusercontent.com` fetches and OS package builds that don't run `make` first still get a usable manpage. Build-side only; no runtime change; no Tiger concern (the build runs on a developer's modern macOS, not on Tiger).
- **Added:** Test-mode `MGA_HOOK_DIR_OVERRIDE` env var + `tests/run_tests.sh` integration test that locks the freeze-hook abort contract from audit finding 5. `src/cmd-fs.c HOOK_DIR` was a hardcoded `#define` to `/etc/qemu/fsfreeze-hook.d`; replaced by a `hook_dir()` getter that honors `MGA_HOOK_DIR_OVERRIDE` ONLY when `test_mode` is enabled (set exclusively by `--test` flag at startup, never attacker-controlled). The script-ownership validation in `run_hooks()` (script must be uid 0, parent dir must be uid 0) is similarly bypassed in test mode — test fixtures live in `/tmp` owned by the test runner. World-writable + executable checks stay enforced in both modes (correctness, not security). Two new test cases: (1) a freeze hook that exits non-zero must produce a `GenericError` with description `Freeze hook script failed`; (2) a freeze hook that exits 0 must NOT abort the freeze (proves the abort is gated on the non-zero exit specifically, not on the mere presence of a hook). Locks finding 5 against future drift. Tiger-compat: `getenv()` is POSIX-ancient, no new APIs. i386/10.4 cross-build clean.
- **Fixed:** `.github/workflows/build.yml` ASAN-integration step previously had `./tests/run_tests.sh ./build/mac-guest-agent-asan || true`, so the job passed even if every integration test failed under ASAN — silently masked any sanitizer-detected bug. Removed the `|| true` so ASAN integration failures fail CI. Verified locally that the ASAN binary (`-fsanitize=address,undefined`) passes the full 75-test integration suite on macOS 26 with the audit-finding fixes in place. Addresses audit.md finding 7.
- **Added:** `.github/workflows/build.yml` — `make test-verify-transports` now runs in the test-matrix job (macOS 14 / 15 / latest) so the shell-shim integration tests catch regressions in CI.
- **Added:** `tests/test_verify_transports.sh` — shell-shim integration test suite for `scripts/verify.sh`. Mocks `qm` + `pvesh` (PVE), runs a real Perl-driven QGA Unix-socket listener (qga-socket / UTM socket I/O), and exercises CLI surface, transport-plugin wiring, JSON appendix schema 2.0, all six optional flags (`--no-redact`, `--no-appendix`, `--no-in-vm`, `--no-env-capture`, `--no-freeze`, `--freeze-cycles N`), redaction (with both raw-present-when-disabled and raw-absent-when-enabled assertions), mount-dispatch cross-check arithmetic (expected vs actual frozen count), and multi-cycle freeze recording (verifies `freeze_cycles_log` length matches `--freeze-cycles`). 57 assertions, ~0.5s runtime, no real hypervisor required. Wired into `make test` via the new `test-verify-transports` Makefile target.
- **Added:** `scripts/verify.sh` multi-cycle freeze test + mount-dispatch cross-check + `--no-freeze` opt-out. The Freeze/Thaw section now runs `--freeze-cycles N` (default 3) consecutive freeze/thaw cycles instead of one — catches state-leak bugs between cycles, which the single-cycle check missed by construction. Each cycle records its own structured entry (cycle number, frozen count, thawed count, fsfreeze-status outcome, behavioural-check outcome, post-thaw outcome, the matching `Filesystem frozen:` log line) in a new `freeze_cycles_log` array in the appendix. The pre-cycle freeze auto-thaw safety trap re-arms and disarms per cycle so a kill between cycles still thaws cleanly. After the last cycle, a `mount_dispatch_crosscheck` runs: from the captured mount table (Host Environment section), it counts mounts whose `fstype` is NOT in `{smbfs, afpfs, nfs, webdav, ftp, devfs, autofs, fdesc, volfs, synthfs, lifs}` and compares to the last cycle's frozen count. PASS if the count is 1..2× the expected (loose because APFS containers can produce more snapshot rows than mount rows and ZFS datasets pad too); FAIL if 0 or grossly over; INFO if the expected count isn't derivable (env-capture off, malformed mount table). New `--no-freeze` flag skips the section entirely for contributors who don't want to freeze a production-ish VM — gives ~80% of the evidence. New `--freeze-cycles N` flag is validated (must be a positive integer) at parse time.
- **Added:** `scripts/verify.sh` Host Environment capture section + JSON appendix schema bumped to 2.0. New section runs **before** Freeze/Thaw (so the captured mount table reflects pre-freeze state and isn't blocked by the freeze command allowlist) and probes the guest via `transport_guest_exec_json` for: `sw_vers -productName -productVersion -buildVersion`; `sysctl -n hw.model hw.ncpu hw.memsize machdep.cpu.brand_string` (machdep.cpu.brand_string is populated on both Intel and Apple-silicon hosts); `kextstat` filtered to `Apple16X50Serial`/`AppleVirtIO`/`IOSerialFamily` families; `ioreg -l -w 0` filtered to serial/virtio nodes (capped at 8 KB); `mount` parsed into `[{device, mount_point, fstype, options}]`; `launchctl list com.macos.guest-agent`; `stat -f "size=%z mtime=%Sm name=%N"` on the agent log file. All captured pieces assembled into a single `host_environment` object embedded in the appendix. Schema bumped to 2.0 (additive change — every 1.0 field is preserved; downstream consumers ignore the new fields are still compatible). New `--no-env-capture` flag opts out for cases where guest-exec is slow or only host-driven checks are wanted. The script's `gx_capture` helper is a thin wrapper around `transport_guest_exec_json` that extracts the `out-data` text, used here and by the existing in-VM diagnostics section.
- **Added:** `scripts/verify.sh` UTM transport via plist-based socket discovery, plus a generic `qga-socket` transport for raw QEMU / custom installs. UTM ships `utmctl` but no arbitrary-QGA subcommand, so the transport talks to the QGA Unix socket directly (same socket `utmctl exec` uses). Discovery reads `~/Library/Containers/com.utmapp.UTM/Data/Documents/<name>.utm/config.plist` via `plutil -convert json -o -`, finds the `Serial` entry with `Interface == "QemuGuestAgent"`, and uses its `Path`. If discovery fails (no QGA serial configured), errors with the exact UTM GUI steps to add one — the .utm bundle is never mutated. `--qga-socket PATH` overrides discovery entirely. Socket I/O uses Perl `IO::Socket::UNIX` + `JSON::PP` (core macOS modules) to avoid BSD `nc`/`socat` version quirks; one helper (`_qga_socket_cmd` + `_qga_socket_guest_exec_json`) is shared by both transports. Preflight refuses to run as root because the UTM socket is owned by the desktop user. The `qga-socket` transport requires `--qga-socket PATH`, validates the path is an actual Unix socket, and probes via `guest-ping` for VM-state detection (no hypervisor metadata to inspect). Both transports return PVE-shape-compatible envelopes from `guest_exec_json` (base64-decoded `out-data`/`err-data`) so the in-VM diagnostics section is transport-agnostic. Auto-detection extended: `utmctl status <id>` exits 0 → UTM; `--qga-socket PATH` set → qga-socket.
- **Added:** `scripts/verify.sh` libvirt transport. Driven by `virsh qemu-agent-command`; reaches `guest-exec` + `guest-exec-status` via the same channel and base64-decodes `out-data`/`err-data` into the same envelope shape PVE's `qm guest exec --output-format json` produces, so the check pipeline above the transport layer is identical between PVE and libvirt. QGA responses unwrapped from libvirt's `{return: ...}` envelope so downstream `json_query` calls work with the same `$d->{field}` shape PVE provides — error envelopes pass through unchanged so the content-based behavioural-freeze check still sees `$d->{error}->{desc}`. Auto-detection: `virsh` on PATH and `virsh dominfo <id>` exits 0. Preflight: virsh+perl+base64 present, libvirtd socket reachable (root or libvirt-group membership; honours `LIBVIRT_DEFAULT_URI`), domain exists. Config check looks for the documented `org.qemu.guest_agent.0` virtio-serial channel in `virsh dumpxml`; without it the in-guest agent has nothing to talk to and the verifier flags it before any real test runs. Discard/SSD-emulation hints best-effort-grepped from the disk XML.
- **Added:** `scripts/verify.sh` — unified, multi-transport host-side verifier replacing `scripts/pve-verify.sh`. Auto-detects PVE (libvirt and UTM land in subsequent commits) from the host environment, or accepts `--transport pve|libvirt|utm|qga-socket` explicitly. Transport plugin architecture: every transport implements five primitives (`transport_describe`, `transport_vm_state`, `transport_config_summary`, `transport_qga_cmd`, `transport_guest_exec_json`); the check pipeline is transport-agnostic above that layer. PVE transport ships in this commit and is a direct port of the prior `pve-verify.sh` flow (config / VM-state / agent comms / memory / freeze-thaw with content-based behavioural check / in-VM `--self-test-json` + `--safe-test-json` / freeze-log fetch / JSON appendix), plus three new safety preflights — root check, PVE cluster locality check (refuses to run when the VM lives on a different node), and PVE backup-lock check (refuses to run when `vzdump` is in progress). New auto-thaw safety trap fires on `EXIT`/`INT`/`TERM` and issues `fsfreeze-thaw` if the script is killed between freeze and thaw — the agent has its own 10-minute auto-thaw safety net, this just makes recovery immediate. PII redaction (IPv4 / MAC / supplied identifier) reimplemented in Perl rather than `sed -E` because BSD sed on macOS doesn't support `\b` word boundaries; one redaction implementation works on Linux and macOS hosts (relevant for the upcoming UTM transport).
- **Removed:** `scripts/pve-verify.sh` deleted (no compatibility shim). The single previous user is on the same branch as this commit; superseded entirely by `scripts/verify.sh --transport pve`.
- **Added:** `scripts/pve-verify.sh` Phase 3 one-shot rewrite. A single host-side invocation (`./pve-verify.sh <vmid>`) now produces the full Tier-2 → Tier-1 evidence: the existing host-side checks (config, VM state, agent ping/get-osinfo/network/info, agent-sourced memory, freeze/thaw round-trip), **plus** in-VM `mac-guest-agent --self-test-json` and `--safe-test-json` driven via `qm guest exec --output-format json` (no need to SSH into the guest or run anything manually inside the VM), **plus** a tail of the agent log for the per-event `Filesystem frozen: ...` INFO line that summarises the per-treatment breakdown (Phase 2 Q3). Output is a human-readable text report followed by a structured JSON appendix that contributors paste straight into `docs/evidence/<version>/pve-verify.json` — appendix embeds the in-VM JSON outputs as parsed objects (`in_vm_selftest`, `in_vm_safetest`) plus host-side check records (`host_checks`) and the freeze-event log line (`freeze_log_tail`). PII (IPv4 addresses, MAC addresses, supplied VM ID) redacted by default; new flags `--no-redact`, `--no-appendix`, `--no-in-vm`, `--agent-path`, `--log-path`, `--exec-timeout`, `--help`. Static-contract check on the in-VM `freeze_dispatch` block: verifies the agent advertises `per_fstypename.apfs = "tmutil_snapshot+f_fullfsync"` and `cpustats_discriminator = "linux"` (Phase 2 Q3/Q4) so contract drift between the binary and `docs/design/FREEZE_SEMANTICS.md` becomes a visible verifier FAIL. Implements all of `docs/PLAN.md` Phase 3.
- **Fixed:** `scripts/pve-verify.sh` frozen-state behavioural check inspected `qm agent get-osinfo` exit code, which is structurally unreliable. Per `docs/research/UPSTREAM_NOTES.md` Target 4, PVE's `register_command` dispatcher (used by `qm agent <cmd>`) wraps QGA errors as `{result:{error:{...}}}` and the CLI exits 0 regardless of whether the agent answered or refused. The check now inspects *response content*: presence of `"pretty-name"` → FAIL (agent answered while frozen); presence of `"Command not allowed while filesystem is frozen"` or `"error"` → PASS (genuinely gated); anything else → INFO with the truncated raw response. Same content-inspection rule applied to the post-thaw "agent responds normally again" check (looks for `"pretty-name"`). Robust regardless of whether future PVE versions change the wrapper.
- **Updated:** `docs/COMPATIBILITY.md` "Step 2: Runtime Validation" and `docs/evidence/README.md` per-version layout updated for the one-shot flow. Step 2 is now install-in-VM (one-time) + `pve-verify.sh` on the host (everything else); the previous "run two commands in the VM" step is gone. Evidence layout prefers `pve-verify.txt` + `pve-verify.json` (split at the `JSON Appendix` header in the script output); the legacy three-file layout (`selftest.json`, `safetest.json`, `pve-verify.txt`) is still accepted, no rewrite of existing per-version directories.
- **Added:** `--self-test-json` now emits a `freeze_dispatch` JSON sibling of `system_info`. Surfaces the per-`f_fstypename` dispatch policy table (apfs → `tmutil_snapshot+f_fullfsync`, hfs → `f_fullfsync`, FAT/exFAT/UDF/NTFS → `f_fullfsync_with_enotsup_tolerated`, ZFS → `zfs_snapshot_if_cli_else_f_fullfsync`, network → `skip_network`, special → `skip_special`), the default log path and the per-event INFO-line prefix that `scripts/pve-verify.sh` greps for, a `zfs_cli_available` boolean (resolved via the same `find_zfs_cli()` cache as the freeze path), the three documented divergences from upstream QGA (`idempotent_re_freeze`, no `persistent_frozen_state_marker`, no `logging_disabled_during_freeze`), and the `cpustats_discriminator` (`"linux"`) so a verifier can statically check that the wire shape of `guest-get-cpustats` matches what the agent advertises. Lets contributors and PVE-side tooling introspect the agent's freeze policy without having to run a real freeze. Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q3. Backed by two new `tests/run_tests.sh` contract checks: (1) `freeze_dispatch` block shape + dispatch-table values, (2) `guest-get-cpustats` discriminator round-trip against the advertised value.
- **Fixed:** `scripts/pve-verify.sh` memory check reported `PASS  memory reporting: 0GB / 0GB`. It read PVE's host-side QMP/balloon counters (blank for macOS guests — macOS ships no virtio-balloon stats driver) by scraping the `pvesh` text table, and printed `PASS` without validating the parsed values. Rewritten: memory now comes from the guest agent itself (`get-memory-block-info` + `get-memory-blocks`), with real used/total derived from its data; agent JSON is parsed with Perl `JSON::PP` instead of `grep`-on-text; the result model is fail-closed, so no check prints `PASS` on data it could not parse; added a `qm`/`perl` preflight and a VM-running check.
- **Fixed:** `scripts/pve-verify.sh` — the `type=isa` config check required `enabled=1` to appear before `type=isa` on the config line, producing a false `FAIL` when Proxmox wrote the agent options in the other order; the two options are now matched independently. The freeze/thaw checks passed on any digit in the output, including a zero-filesystem freeze; they now require a parsed count of at least 1.
- **Added:** `scripts/pve-verify.sh` freeze check now verifies the frozen *state* behaviourally — while frozen, the agent must reject a non-freeze command (`get-osinfo`), and must resume normal operation after thaw — rather than trusting `fsfreeze-status`, which only echoes the agent's internal frozen flag. macOS has no `FIFREEZE`, so the rejection behaviour is the observable proof that the freeze took effect.

### Documentation
- **Added:** `docs/evidence/10.11.6/` — first real-world v2.4.3 evidence drop. Apple Xserve3,1 (real bare metal, not emulated), Mac OS X 10.11.6, HFS+ on `/dev/disk0s2`, PVE host-side `verify.sh` run reporting 38 passed / 0 failed. Captured `host_environment` (sw_vers / Xserve3,1 hardware / Intel Xeon W5590 / 8 GiB / IOSerialFamily v11 + Apple16X50Serial v3.2 + Apple16X50ACPI v3.2 / mount table / launchd state / log-file stat), three freeze cycles with per-cycle log line (`sync + F_FULLFSYNC` on the HFS+ root, 4 special FS categorically skipped — matches `docs/design/FREEZE_SEMANTICS.md`), mount-dispatch cross-check passed, `freeze_dispatch` JSON contract validated against the binary, `--self-test-json` 20/0/0, `--safe-test-json` 21/21. COMPATIBILITY.md row for 10.11 El Capitan refreshed with the v2.4.3 evidence reference.
- **Fixed:** `scripts/verify.sh` — two real-world fixes from the El Cap evidence run. (1) `qm guest exec --output-format json` is not supported on some PVE versions in the wild (returns `400 unable to parse option`); the default output is already JSON, so the flag was dropped. The silent failure mode previously reported "guest-exec failed (binary missing or guest-exec disabled?)" even though guest-exec worked — misleading. (2) `--safe-test-json`'s real wire shape is top-level `{passes, failures, status, agent_version, test}` with no nested `summary` and no `total`; the parser was looking for `summary.passed / summary.failed`. Synthesised `total = passes + failures` and added the `status` field to the human message. The shell-shim test fixture was updated in lockstep to emit the real shape (it had been emitting my fabricated shape, so the integration suite was passing for the wrong reason).
- **Fixed:** Docs and code disagreed about freeze-hook failure handling — `SECURITY.md` and `configs/hooks/README.md` both stated "a hook script failure does not abort the freeze", but `src/cmd-fs.c run_hooks()` has aborted the freeze on freeze-hook non-zero exit since at least v2.4.0 (`if (strcmp(action, "freeze") == 0) failed = 1`). The code is the right default — a hook's purpose is typically to flush in-flight writes for backup consistency (`FLUSH TABLES WITH READ LOCK`, `CHECKPOINT`, `BGSAVE`), so a failed flush means the snapshot is inconsistent for that workload and surfacing it as a freeze failure is the safer choice (matches the fail-secure / strict-default pattern established in Phase 2 / findings 1–4 here). Aligned both docs to the code. Also documented the asymmetric thaw-hook behaviour (thaw hooks log on non-zero but always proceed — refusing to thaw would leave the VM filesystem indefinitely frozen) and the validation-failure path (wrong owner / world-writable / not-executable scripts are skipped before they run; that's a configuration error, distinct from a runtime non-zero exit). Addresses audit.md finding 5. The audit's "add an integration test with a failing hook" suggestion is deferred — requires `HOOK_DIR` to be runtime-overridable (currently a hardcoded `#define` to `/etc/qemu/fsfreeze-hook.d`) AND test-mode bypass of the root-ownership validation; both are real changes that haven't been requested for the next release.
- **Added:** `docs/design/FREEZE_SEMANTICS.md` — single source of truth defining what `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` actually do per `f_fstypename` on macOS, what each treatment guarantees, the five documented divergences from upstream QEMU Guest Agent (idempotent re-freeze, no persistent frozen-state marker, logging-during-freeze, `guest-sync-id` extension, foreign-FS `F_FULLFSYNC` failure treatment), the freeze-time command allowlist contract, and the must-surface vs by-design failure-mode classification. The dispatch table is the same one expressed in `fs_dispatch_class()` and surfaced in `--self-test-json`'s `freeze_dispatch` block — one source of truth (the code), one verbatim copy in the doc, one verbatim copy in the JSON envelope. Linked from `README.md` Documentation index and from `docs/BACKUP.md` "What 'freeze' means per filesystem". Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q7c.
- **Fixed:** `docs/PVE.md` "Accurate Memory Reporting Without Balloon Driver" section misleadingly implied that installing the agent makes PVE's web UI memory gauge accurate on macOS guests. It doesn't — `pvestatd` and the PVE web UI source per-VM memory from the virtio-balloon device's stats vq (when populated) or from the cgroup RSS of the QEMU process scope (always), and they never call the guest agent for memory. macOS ships no virtio-balloon driver on any version, so the balloon path is empty and the gauge falls back to cgroup RSS — and installing this agent doesn't change that. Section retitled "Memory reporting on macOS guests" and rewritten to honestly describe: (a) what the gauge actually reads (cgroup RSS, structural balloon-driver limitation), (b) what this agent does provide (guest-side memory view via `guest-get-memory-blocks` / `guest-get-memory-block-info`, consumable directly via `qm agent <vmid> get-memory-blocks` or rendered by `scripts/pve-verify.sh`), (c) why reclamation is impossible regardless (no balloon driver to inflate). Cross-referenced from `docs/research/UPSTREAM_NOTES.md` Targets 5 and 7. Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q7a.
- **Fixed:** `README.md` (two callouts), `docs/COMPATIBILITY.md` (new "ISA Serial Transport — Why" section + architectural-transitions row), and `src/channel.c` (`known_devices[]` ISA-block comment) — the "ISA because Apple claims VirtIO" rationale was right for Apple Virtualization.framework hosts (UTM/`vz_run`, where Apple's `AppleQEMUGuestAgent` is IOKit-launched on the VirtIO console channel via the `AppleVirtIOAgentDevice` match set by `applevirtio.console`) but oversimplified for plain QEMU/KVM hosts (Proxmox/libvirt/raw QEMU, typically OpenCore-booted, where `applevirtio.console` doesn't load and Apple's agent never launches — leaving the VirtIO console channel actually free). All four sites updated to articulate both host classes and explain why we still default to ISA universally: one transport across both host classes (no IOKit introspection at startup, identical launchd plist and channel-detection list everywhere), and no conflict if a disk image is moved between QEMU and VZ. Backed by `docs/research/UPSTREAM_NOTES.md` Target 6 (local Mach-O symbol survey of `/usr/libexec/AppleQEMUGuestAgent` on macOS 26.5). Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q7b.
- **Updated:** `docs/BACKUP.md` — "How Freeze Works" and "Freeze Methods by macOS Version" sections reflect the v2.4.3 per-FS dispatch (replacing the prior "10.4–10.12: sync+F_FULLFSYNC / 10.13+: sync+F_FULLFSYNC+APFS snapshot" two-row table that hid the foreign-FS, ZFS, network-mount, and special-FS treatments). The stale "**Note on `guest-fsfreeze-freeze-list`:** This command accepts a mountpoint list parameter but currently freezes all filesystems regardless — the mountpoint filter is not yet implemented" is replaced with a description of the new subset-freeze handler (including the deliberate skip of the container-level APFS snapshot for subset requests). The freeze-time command allowlist is now spelled out (9 commands; the upstream 6 plus three documented divergences) instead of the vague "ping, sync, info, freeze/thaw allowed". All elaboration links back to `docs/design/FREEZE_SEMANTICS.md` as the canonical reference.
- **Updated:** `docs/COMPATIBILITY.md` — promoted **10.4 Tiger** to Tier 1 after @vit9696's v2.4.2 confirmation (issue #2): agent serves PVE end-to-end on 10.4.11 (ping, get-osinfo, network, memory, reboot/shutdown). Matches the convention 15.7 Sequoia already set (Tier 1 with freeze untested).
- **Updated:** `docs/COMPATIBILITY.md` "Step 2: Runtime Validation" sequence now points at `scripts/pve-verify.sh` (one-shot host-side validation with agent-sourced memory + behavioural freeze check) and the modern `--self-test-json` + `--safe-test-json` in-VM diagnostics, replacing the older `tests/safe_test.sh` reference. Added a note on how external contributors submit results (issue comment or PR under `docs/evidence/<version>/`).
- **Fixed:** `docs/CLI.md` Device Auto-Detection section listed the probe order as VirtIO → UTM → ISA. The code in `src/channel.c` has been ISA-first since v2.1.0 — deliberately, because Apple's built-in VirtIO guest agent on Big Sur+ claims the VirtIO channel and ISA is the only one it leaves alone. Reordered the doc to match the code and the v2.1.0 rationale.
- **Added:** `docs/evidence/` directory with a README defining the per-version layout (`selftest.json`, `safetest.json`, `pve-verify.txt`, optional `NOTES.md`) and the submission flow referenced from the reply to issue #2 — so contributors land on a real path with format guidance instead of an empty directory.
- **Added:** `docs/PLAN.md` — phased roadmap (research → configuration matrix and intent design → one-shot validator) covering the deeper freeze/gating/foreign-FS gaps surfaced by @vit9696's Tier-2 submission on 10.4.11. Scaffolded `docs/research/UPSTREAM_NOTES.md` to capture Phase 1 evidence (QGA spec, Linux reference impl, PVE wrapper behaviour, etc.) before any code change.

### Bug Fixes
- **Improved:** `ssh_safe_write_file()` now uses an atomic temp-file-plus-rename pattern instead of open-truncate-then-write. The prior implementation refused to follow a symlink at the target (good — closed the audit finding 6 privesc) but left two operational weaknesses: (a) if the agent crashed mid-write, the user's `authorized_keys` was permanently corrupted and SSH access was lost until manually restored; (b) any reader (sshd checking the file for the next authentication, a backup utility, anything) racing the write would see a partial file. The new pattern writes to `<dir>/.<basename>.tmp.<pid>` via `O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW`, `fchown`/`fchmod` by fd, write loop, then `rename(2)` over the target — `rename()` is POSIX-atomic for same-directory same-filesystem operations, so readers never see a partial file, and a mid-write crash leaves the target's prior content intact (only a stray dot-file in `.ssh/` that the operator can clean up). As a side benefit, a pre-positioned symlink at the target is now atomically *replaced* with a regular file rather than the write being refused — keeps the user's SSH access working instead of leaving the broken symlink in place. Same Tiger-compat constraints as the rest of audit finding 6's hardening: every primitive (`O_EXCL`, `rename`, `unlink` etc.) is POSIX.1-2001, i386/10.4 cross-build clean. `tests/test_proactive.c` updated to assert the new atomic-replacement semantics (symlink replaced rather than refused, victim file still untouched, no leftover temp file after success). Error messages in `cmd-ssh.c` updated for the new failure mode ("temp create or rename failed; .ssh directory may have wrong permissions").

- **Fixed (security):** SSH key management followed symlinks as root — the audit's finding 6 demonstrated a real privilege-escalation surface. `guest-ssh-add-authorized-keys` and `guest-ssh-remove-authorized-keys` both wrote `<home>/.ssh/authorized_keys` via plain `open(O_WRONLY|O_CREAT|O_TRUNC)` and then `chown()`d the path by name; `guest-ssh-get-authorized-keys` read it via `fopen()`. None of those calls used `O_NOFOLLOW`. A user with write access to their own home directory could replace `~/.ssh/authorized_keys` with a symlink to any root-owned file (`/etc/shadow`, `/var/db/dslocal/nodes/Default/users/root.plist`, etc.) and trick the root-running agent into truncating, chowning to that user, or exposing the content of the linked-to file via the QGA response. Hardened all three handlers in `src/cmd-ssh.c` via three new file-local helpers (exposed for unit testing): `ssh_safe_read_file` opens with `O_RDONLY|O_NOFOLLOW` and `fstat`-rejects non-regular files before reading; `ssh_safe_write_file` opens with `O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW` and calls `fchown`/`fchmod` BY FD (not by path), so any racing symlink swap between open and metadata change still only mutates our held fd's inode; `ssh_safe_ssh_dir` creates `~/.ssh` via `mkdir(0700)` (which itself doesn't follow symlinks on the final component) and verifies the result via `lstat` + `S_ISDIR` before `lchown`ing (instead of the prior `chown` which would have followed). Tiger-compatible — every primitive (`O_NOFOLLOW`, `fchown`, `fchmod`, `lchown`, `lstat`) is POSIX.1-2001 and shipped on macOS since 10.0; i386/10.4 cross-build clean. Doesn't use `openat`/`renameat` (POSIX.1-2008, macOS 10.5+) — atomic rename is a future improvement when we drop 10.4. New `tests/test_proactive.c` regression: 10 assertions covering (a) write to symlinked target → `-1` with victim file untouched, (b) write to non-existent target → succeeds with `0600` regular file containing the input, (c) write to existing regular file → truncate-and-rewrite succeeds, (d) read of symlinked target → `NULL` (no content exposure), (e) read of regular file → returns the data. Test wiring: `cmd-ssh.c` added to the `test-proactive` Makefile link line; `command_register` stub already in place from earlier. Addresses audit.md finding 6.
- **Fixed:** Version metadata was inconsistent across files — `Makefile` said `2.4.2`, `scripts/build-pkg.sh` hardcoded `2.4.0`, `docs/mac-guest-agent.8` said `2.2.0`, and `docs/BACKUP.md` referred to behaviour "since v2.4.3" (the unreleased target). The release workflow extracted a tag-derived `$VERSION` into the env but didn't pass it through to `make`, so a tagged release could publish binaries whose embedded version came from the Makefile rather than the git tag. Addresses audit.md finding 4. Fixes: Makefile bumped from 2.4.2 → 2.4.3 (matches the docs and the work shipped in this Unreleased section); `scripts/build-pkg.sh` now reads `VERSION` from the Makefile via `awk '/^VERSION[[:space:]]*:=/{print $2}'` with a `VERSION` env override (used by the release workflow to stamp the tag version); `docs/mac-guest-agent.8` bumped to 2.4.3 with a `.\"` comment noting the sync requirement and pointing at the future improvement (stamping it from `$(VERSION)` at build time); `.github/workflows/release.yml` now invokes `make VERSION="$VERSION" build-all`, `make VERSION="$VERSION" build-i386`, and `make VERSION="$VERSION" build` so tagged-release binaries always carry the git-tag's version. Single source of truth is the Makefile; the .pkg script and the release workflow both honour `VERSION` env overrides for explicit ad-hoc bumps. Binary `--version` confirmed reports `mac-guest-agent 2.4.3` after the bump.
- **Fixed:** `base64_decode()` accepted any input whose length was a multiple of 4, including bytes outside the base64 alphabet — characters not in `[A-Za-z0-9+/=]` mapped to `0` in the lookup table (which means literal `A`), so unvalidated input like `"!!!!"` silently decoded to three zero bytes. Affected `guest-file-write` (would write zero bytes for any non-base64 input the caller sent, silently corrupting the file) and `guest-set-user-password` with `crypted=false` (would either silently use zero bytes as the password OR — separately — fall through to use the raw literal string as the password if decoding "succeeded" in the prior loose sense). Tightened `src/util.c base64_decode()`: every non-padding character is now validated against the `[A-Za-z0-9+/]` alphabet, and `=` is allowed only in the last 1 or 2 positions of the input (RFC 4648 §3.2). Invalid alphabet, embedded whitespace, high-bit bytes, URL-safe substitutions (`-`/`_`), three-or-more `=`, and `=` anywhere but the tail all now return `NULL` (which both existing callsites already treat as "decode failed → return GenericError"). `src/cmd-user.c handle_set_user_password` additionally now returns an `InvalidParameter` error when `crypted=false` and base64 decoding fails — the prior silent-fallthrough-to-raw-literal would have set the user's password to whatever literal bytes the caller happened to send. 28 new unit-test cases in `tests/test_proactive.c` cover round-trip of known inputs, every category of invalid alphabet, every category of bad padding, every category of bad length, and the NULL safety guard. Addresses audit.md finding 3.
- **Fixed:** `guest-get-diskstats` emitted iostat-style fields at the top level (`name`, `kb-per-transfer`, `transfers-per-second`, `mb-per-second`) — the QGA `GuestDiskStatsInfo` schema wants `{name, major, minor, stats: {15 Linux-block-stats fields}}`. Strict QGA consumers (virsh / PVE plugins) reject the prior shape. Rewritten in `src/cmd-disk.c handle_get_diskstats()` to source real cumulative per-disk counters from IOKit's `IOBlockStorageDriver` `Statistics` property dict instead of parsing `iostat` rate snapshots — 6 of the 15 spec fields map cleanly (`read-sectors` ← `Bytes (Read)` / 512, `read-ios` ← `Operations (Read)`, `write-sectors` ← `Bytes (Write)` / 512, `write-ios` ← `Operations (Write)`, `read-ticks` ← `Total Time (Read)` ns → ms, `write-ticks` ← `Total Time (Write)` ns → ms). The remaining 9 Linux-block-layer-specific fields (`read-merges`, `write-merges`, `discard-sectors`, `discard-ios`, `discard-merges`, `discard-ticks`, `in-flight`, `io-ticks`, `time-in-queue`) emit `0` — same honest-zero precedent as cpustats `nice: 0` and route `metric: 0` / `irtt: 0`. `major`/`minor` also `0` (macOS has no stable Linux-style block-device major/minor numbers). BSD device name discovered by recursively walking the IOBlockStorageDriver's children for the `BSD Name` property on the child IOMedia node (via `IORegistryEntrySearchCFProperty(... kIORegistryIterateRecursively)`). New helper `cfdict_u64()` reads a uint64 out of a CFDictionary by C-string key. New includes: `<stdint.h>`, `<CoreFoundation/CoreFoundation.h>`, `<IOKit/IOKitLib.h>`, `<IOKit/IOBSD.h>`, `<IOKit/storage/IOBlockStorageDriver.h>`, `<IOKit/storage/IOMedia.h>` — `IOKit` and `CoreFoundation` frameworks were already linked. New `tests/run_tests.sh` shape contract validates the full 4-top + 15-stats field set per entry. `docs/COMMAND_STATUS.md` row promoted from "caveated/partial — Returns raw iostat output" to "stable/partial — IOKit IOBlockStorageDriver Statistics". Addresses audit.md finding 2c. **Breaking for any caller that hard-coded the prior iostat-style field names.**
- **Fixed:** `guest-network-get-route` route objects didn't match the QGA `GuestNetworkRoute` schema — emitted `destination` / `nexthop` / `source` / `interface` / `version` / `prefix`, missing the spec's `iface` / `gateway` / `mask` / `metric` / `irtt` / `desprefixlen`. Strict virsh / qm-agent / PVE-plugin consumers reject responses with the prior field names. Rewritten in `src/cmd-network.c handle_network_get_route()` to emit the spec shape: `iface`, `destination` (stripped of /CIDR; "default" normalised to `0.0.0.0` / `::`), `gateway`, `nexthop` (alias for `gateway`, which the QGA schema also defines), `mask` (computed from the prefix length — IPv4 dotted-quad like `255.255.255.0` or IPv6 colon-hex like `ffff:ffff:ffff:ffff:0000:...`), `metric` (constant `0` — macOS `netstat -rn` doesn't expose a metric column), `irtt` (constant `0` — Linux-only concept), `version`, `desprefixlen`. The `0` defaults for `metric` and `irtt` follow the same precedent as `guest-get-cpustats`'s `nice: 0` on macOS (Q4 / audit finding 2a pattern: spec-conformant with honest zeros for fields the host can't supply). Two new helpers: `ipv4_prefix_to_mask()` and `ipv6_prefix_to_mask()`. `<stdint.h>` added for `uint32_t`. `tests/run_tests.sh` shape contract updated to assert the full set of spec fields (`iface`, `destination`, `gateway`, `nexthop`, `mask`, `metric`, `irtt`, `version`, `desprefixlen`). Addresses audit.md finding 2b. **Breaking for any caller that hard-coded the prior field names.**
- **Fixed:** `guest-get-load` returned `load1`/`load5`/`load15`; the QGA `GuestLoadStats` schema requires `load1m`/`load5m`/`load15m` (the `m` suffix marks "minutes"). Strict QGA parsers reject the prior field names. Renamed in `src/cmd-system.c` to match the spec. `tests/run_tests.sh` shape contract + `tests/safe_test.sh` field probe + print statement updated. Addresses audit.md finding 2a. **Breaking for any caller that hard-coded the prior field names** — none of the in-tree consumers did (the safe-test paths above are the only references), and the rename is a textual change only (same three doubles, same semantics).
- **Fixed:** `guest-exec` was synchronous and could deadlock on stderr-heavy children — addressed audit.md finding 1. The old `handle_exec()` drained stdout to EOF, then stderr to EOF, then `waitpid()`-blocked the entire agent main loop until the child exited. Two real failures: (1) a child writing more than the ~64 KB pipe buffer to stderr while stdout stayed small would deadlock — child blocked on stderr write, parent blocked on stdout read, both waiting forever; (2) every QGA command from the host (ping, freeze, network checks, status polls from other callers) stalled for the child's entire lifetime. Neither matches the QGA spec, which has `guest-exec` return `{pid: N}` immediately and `guest-exec-status` poll for completion. Rewritten to the spec contract: `handle_exec()` forks, sets the parent's pipe read ends non-blocking via `fcntl(F_SETFL, O_NONBLOCK)`, stores the fds + per-stream accumulating buffers + truncation flags in the process table, returns `{pid: N}` immediately. A new `drain_one_fd()` helper does nonblocking `read()` chunks until `EAGAIN`/`EOF`/error; `cmd_exec_drain_all()` (called from the agent main poll loop on every wake-up tick) keeps in-flight children's pipes from backing up while the caller is between status polls; `handle_exec_status()` opportunistically drains the named pid, reaps via `waitpid(WNOHANG)`, returns the current state. Output is base64-encoded only at status-return time. Same `MAX_CAPTURE_SIZE = 16 MB` per stream, same `out-truncated`/`err-truncated` flags as upstream Linux qemu-ga and Windows qemu-ga (matches their async model — Linux uses GLib I/O callbacks, Windows uses one reader thread per pipe, ours uses event-driven nonblocking drain). Zero compatibility risk: only POSIX-classic syscalls (`fork`/`pipe`/`fcntl F_SETFL O_NONBLOCK`/`waitpid WNOHANG`), all available on Mac OS X 10.0+ — i386/10.4 cross-build clean. Two new `tests/run_tests.sh` regression tests: (a) `sleep 2` with `capture-output=true` returns from `guest-exec` within 250 ms (was the agent blocking 2 seconds); (b) `dd if=/dev/zero bs=4096 count=64 1>&2` (256 KB to stderr — 4× the pipe buffer) completes end-to-end without deadlock and the captured `err-data` matches. The existing exec tests were also rewritten to poll `guest-exec-status` instead of assuming sync semantics from the immediate response.
- **Fixed:** CI static-analyzer false positive `cmd-fs.c:253:14: The 1st argument to 'open' is NULL but should not be NULL [unix.StdCLibraryFunctions]` introduced when Phase 2 added `try_fullfsync()`. `mnt->f_mntonname` is a fixed `char[MAXPATHLEN]` array inside `struct statfs` and can never be NULL — but the analyzer can't prove that without an explicit non-NULL constraint on `mnt` itself. Added `__attribute__((nonnull))` to `try_fullfsync`'s declaration: documents the precondition (always satisfied — callers pass `&mntbuf[i]` from `sync_all_volumes`) and silences the warning. Verified locally with `clang --analyze` on every `src/*.c` (no warnings).
- **Added:** `zfs snapshot` support for OpenZFS-on-macOS mounts during freeze. Prefers `zfs snapshot <pool>/<dataset>@mac-guest-agent-<timestamp>` over `F_FULLFSYNC` for ZFS-typed mounts — ZFS snapshots are atomic and are the real consistency primitive for ZFS, whereas `F_FULLFSYNC` isn't documented to be implemented on it. The `zfs` CLI is detected lazily at common installation paths (`/usr/local/sbin/zfs`, `/usr/local/bin/zfs`, `/opt/local/bin/zfs`, `/opt/homebrew/bin/zfs`); if absent, the dispatch falls through to `F_FULLFSYNC` as defence in depth. Snapshot names tracked so `do_thaw` can `zfs destroy` them via the matching cleanup path (mirrors how APFS snapshots are tracked + cleaned). Also fixed: `fs_dispatch_class` previously ran the `/dev/` defensive-backing check BEFORE the type check, which meant ZFS mounts (whose `f_mntfromname` is `pool/dataset`, not `/dev/...`) were wrongly classified as `SKIP_SPECIAL`. The dispatch now matches known writable types (`apfs`/`zfs`/`hfs`) first; the `/dev/` check applies only to unknown types. New unit-test cases lock the corrected dispatch order. Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q1 (ZFS).
- **Fixed:** `guest-get-cpustats` returned a flat aggregate object `{user, system, idle, nice}` summed across vCPUs. The QGA spec defines the response as `['GuestCpuStats']` — an array of per-CPU discriminated-union records with a required `type` field, a `cpu` index, and the per-CPU `user`/`nice`/`system`/`idle` tick counters. Our shape was structurally invalid against the schema at the array level. Rewritten using `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` to produce one entry per vCPU. Each entry tagged `type:"linux"` (the only currently-defined value in upstream `GuestCpuStatsType`; emitting an unknown enum value or omitting the field would be rejected by strict QAPI parsers, see `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q4 for the full reasoning). User/system/idle/nice tick semantics translate cleanly between macOS's `processor_cpu_load_info_t` and Linux's `GuestLinuxCpuStats`. New `tests/run_tests.sh` shape contract verifies array structure, per-entry fields, and `type:"linux"` discriminator. `src/selftest.c`'s safe-test expectation for the command updated from `expect_array=0` to `expect_array=1`.
- **Fixed:** `guest-fsfreeze-freeze-list` silently ignored its optional `mountpoints` argument because it shared the global-freeze handler. A caller asking to freeze only `["/Volumes/data"]` got a global freeze instead of the requested subset. The command now has a distinct handler that parses `args.mountpoints`, validates each entry is a string, and restricts the per-FS dispatch loop to mounts whose `f_mntonname` matches one of the listed paths. With no argument or an empty array it delegates to global freeze (the spec's default). Subset freezes deliberately skip the container-level APFS `tmutil localsnapshot` (snapshotting a whole container for a per-mount request would capture state the caller didn't ask us to capture); per-mount `F_FULLFSYNC` is the consistency mechanism for subset freezes. Implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q2. Four integration tests in `tests/run_tests.sh` cover no-args delegation, empty-array delegation, two-mountpoint filter plumbed through (test-mode return matches `n_mountpoints`), and non-string-entry spec-shaped error.
- **Fixed:** Filesystem freeze treated `F_FULLFSYNC` returning `ENOTSUP`/`EOPNOTSUPP` on foreign filesystems (FAT32, exFAT, older MS-DOS drivers, third-party FUSE) as a failure — logging `WARN` and not counting the volume. Per Apple's `fcntl(2)` documentation `F_FULLFSYNC` is only implemented on HFS, MS-DOS (FAT), UDF, and APFS, so the failure is by-design on filesystems that don't implement it; the QEMU Linux QGA reference handles the analogous `EOPNOTSUPP` from `FIFREEZE` by skipping silently. `sync_all_volumes` now dispatches per `f_fstypename`: APFS gets the `tmutil` snapshot + `F_FULLFSYNC`, HFS+ gets `F_FULLFSYNC`, foreign FS tries `F_FULLFSYNC` and counts `ENOTSUP` as `flushed_only` (the global `sync()` at the top already flushed dirty buffers), network mounts (smbfs/afpfs/nfs/webdav) and special FS (devfs/autofs/fdesc/synthfs/volfs/lifs) are skipped categorically. The handler emits a single INFO log line summarising the per-treatment breakdown (snapshotted / zfs_snapshotted / fullfsynced / flushed_only / skipped). The wire response remains a spec-conformant int (sum of "did-something" counters). Reported by @vit9696 in #2; implements `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q1 + Q3 (log). 21-case unit test for `fs_dispatch_class` in `tests/test_proactive.c`.
- **Fixed:** `guest-get-memory-blocks` fabricated a memory-usage figure when `vm_stat` could not be read. `handle_get_memory_blocks` initialised `used` to `total / 2` and only overwrote it on success, so a `get_vm_stat()` failure silently returned a block list implying exactly 50% RAM used — indistinguishable from a real reading. It now returns a QGA error (`Failed to read memory statistics`) instead of a fabricated value. `guest-get-memory-block-info` (block size) is unaffected; it does not depend on `vm_stat`.

### Internal
- **Refactored:** `fsfreeze_command_allowed` split into a pure-function variant `fsfreeze_is_allowlisted` (checks only the allowlist; ignores `freeze_status`) plus the existing public function that wraps it with the state check. Lets tests exercise the allowlist contract in isolation without needing to manipulate the frozen state. The expanded comment on `fsfreeze_command_allowed` states the principled-restrictive rule from `docs/design/AGENT_BEHAVIOUR_SPEC.md` Q5 (allow during freeze iff handler is read-only, doesn't exec, doesn't change agent state) and notes the deliberate divergences from upstream (`guest-sync-id` extension + idempotent re-freeze). No behaviour change: the same 9 commands are allowed during freeze as before. 30 new unit tests in `tests/test_proactive.c` lock the contract — 9 allowed + 20 representative blocked (writes, exec, suspends, read-only commands deliberately NOT on the list) + NULL guard.

## v2.4.2 (2026-05-22)

### Bug Fixes
- **Fixed:** Agent never connected on Mac OS X 10.4 Tiger — the serial transport used `poll()`, which returns `POLLNVAL` (0x20) for the serial device on Tiger. macOS `poll()` is implemented on top of kqueue, and Tiger's serial BSD client does not support the kqueue readiness path, so `poll()` reported a valid, open `/dev/cu.serial1` as invalid. The agent treated that as a fatal device error and reconnect-looped every 5 s without ever reading a command — the host saw `QEMU guest agent is not running`. The serial read and write paths in `channel.c` now use `select()`, which uses the legacy `selrecord` path the driver implements and works on every macOS version. Reported by @vit9696 in #2.

### Testing
- **Added:** `tests/test_proactive.c` — channel read over a real PTY, covering the `select()`-based read path (framed-message read and idle timeout). The transport read path previously had no behavioral test coverage.

## v2.4.1 (2026-05-20)

### Bug Fixes
- **Fixed:** `--safe-test` crash on Mac OS X 10.4 Tiger (dyld lazy-bind failure on `_host_statistics64`). Weak-import the symbol so the existing `vm_stat` text fallback in `get_vm_stat()` actually runs on 10.4 instead of the process aborting before the runtime check fires. Reported by @vit9696 in #2.

### Documentation
- **Fixed:** `docs/COMPATIBILITY.md` — `host_statistics64` was incorrectly listed as present on Tiger. It was introduced in 10.6 Snow Leopard. Symbol list and Tiger row corrected; Tiger now noted as relying on the `vm_stat` text fallback for memory stats.
- **Fixed:** `scripts/verify-installer.sh` — `host_statistics64` moved from required to optional in the symbol audit, matching what the binary now actually needs.
- **Added:** Tiger / Leopard PATH note in README — `/usr/local/bin` is not in the default PATH on 10.4–10.5; users should invoke via absolute path or `export PATH=/usr/local/bin:$PATH`.

## v2.4.0 (2026-03-28)

### New Features
- **`--safe-test` / `--safe-test-json`** — built-in read-only command validation. 21 tests, no external script or python needed. Run `sudo mac-guest-agent --safe-test` to verify all read-only commands work correctly.
- **`scripts/pve-verify.sh`** — host-side verification script. Run from PVE host against a VM ID to check config, ping, OS info, network, command count, memory reporting, and freeze round-trip.

### Security Hardening (25 findings addressed)
- **Fixed:** Stop deleting ALL Time Machine snapshots on freeze — now only deletes the snapshot we created
- **Fixed:** Shutdown returns error when fork fails (was silently returning success)
- **Fixed:** SSH key removal returns error when write fails (was silently returning success)
- **Fixed:** Save/restore hibernatemode around suspend (was permanently altered)
- **Fixed:** NULL dereference before null check in channel_create_test
- **Fixed:** Unchecked realloc in SSH key operations (crash on OOM)
- **Fixed:** Memory leak in freeze hook cleanup (empty loop body)
- **Fixed:** Output capture capped at 16MB (matches Linux qemu-ga)
- **Fixed:** tmutil snapshot deletion uses run_command_v (no shell injection)
- **Fixed:** selftest tool_available uses access() instead of system()
- **Fixed:** Signal handler uses volatile sig_atomic_t flag
- **Fixed:** Password zeroed in all code paths with compiler-safe secure_zero
- **Fixed:** setenv() instead of putenv() after fork in guest-exec
- **Fixed:** base64_encode overflow guard for 32-bit
- **Fixed:** json_escape handles control characters
- **Fixed:** Unsupported commands (set-vcpus, set-memory-blocks) registered as disabled
- **Fixed:** LOG_FATAL no longer calls exit() — caller handles cleanup
- **Fixed:** guest-get-diskstats returns structured per-disk stats (was raw text)
- **Fixed:** commands_init guard prevents double-registration

### CI/CD
- Fixed: macos-26 replaced with macos-latest (valid runner)
- Version now single-sourced from Makefile (agent.h uses -DVERSION)

## v2.3.1 (2026-03-28)

### Bug Fixes
- **Fixed:** Malformed JSON input now returns a proper error response per QMP spec instead of being silently discarded. Found by pgcudahy (PR #1).
- **Fixed:** Device detection error message now says "No serial device found" with setup instructions instead of the misleading "No virtio device found."

### Critical Documentation Fix
- **`type=isa` is required on ALL macOS versions.** macOS Big Sur+ ships Apple's own built-in VirtIO guest agent (~18 commands) which claims the default VirtIO serial channel. Using `agent: enabled=1` (default) connects to Apple's agent, not ours — losing freeze, memory reporting, and 27 other commands. ISA serial is the only channel Apple's agent doesn't claim.
- Full comparison of Apple's agent (18 commands) vs ours (45 commands) added to docs/PLATFORMS.md.

### Changes
- ISA serial now checked first in device detection order (was last)
- Run_tests.sh: malformed JSON and missing execute tests un-skipped (65 tests, up from 63)
- PVE.md: "existing VM" troubleshooting for users adding agent to klabsdev-style setups
- LIBVIRT.md: VirtIO channel examples replaced with ISA serial (required)
- COMPATIBILITY.md: Sequoia 15.7.5 promoted to Tier 1 (first external user confirmation)
- COMPATIBILITY.md: PPC status and path to support documented

## v2.3.0 (2026-03-25)

### New Command
- **`guest-network-get-route`** — IPv4 and IPv6 routing table via `netstat -rn`. Achieves 100% Linux qemu-ga command parity (45 commands; only `guest-get-devices` unimplemented, which is Windows-only).

### New Features
- **`--self-test` and `--self-test-json`** — environment diagnostics with backup readiness check. Reports freeze method, kext version, APFS/VirtIO capabilities, hook validation, and overall backup readiness verdict.
- **Backup readiness section** in self-test: freeze method (APFS snapshot / sync / sync-only), root capability, hook count, overall verdict.
- **i386 binary** — cross-compiled via MacOSX10.13.sdk for Tiger (10.4) and Leopard (10.5) support.
- **Baud rate set to 115200** — explicit max baud rate on serial port. QEMU ignores baud rate on virtual serial, but macOS kext may use it for internal pacing.

### Platform Support
- **UTM** — auto-detects `/dev/cu.virtio` (Apple Virtualization.framework)
- **libvirt/virt-manager** — domain XML for ISA serial and VirtIO channels, virsh command examples, quiesced snapshots
- **VirtIO prioritized over ISA serial** on Big Sur+ (native driver preferred when available)
- Device detection order: VirtIO (QEMU/PVE/libvirt) → UTM → ISA serial (fallback)

### Documentation
- **Restructured README** — quick-start focused, detailed content moved to docs/
- **docs/PVE.md** — complete Proxmox VE operational guide with troubleshooting
- **docs/LIBVIRT.md** — full libvirt/virt-manager deployment guide with domain XML examples
- **docs/UTM.md** — UTM guide with utmctl comparison, CI/CD workflows, headless automation
- **docs/BACKUP.md** — freeze mechanics, hook scripts, TRIM guide
- **docs/CLI.md** — all flags, config file, device auto-detection
- **docs/PLATFORMS.md** — platform index with transport priority
- **configs/hooks/** — ready-to-use freeze hooks for MySQL, PostgreSQL, Redis, launchd services
- **configs/pve/** — anchor VM configurations for Tiger, High Sierra, Big Sur, Sequoia

### Compatibility
- **18 macOS versions researched** (10.4 Tiger through 26.3 Tahoe)
- **Apple16X50Serial.kext** verified present on every version with identical PCI class match
- Kext version timeline: v1.6 (Tiger base) → v1.7 (Tiger Intel 10.4.5) → v1.9 (Tiger 10.4.11 combo / Leopard) → v3.0 (Snow Leopard / Lion) → v3.1 (Mountain Lion) → v3.2 (Mavericks through Tahoe)
- **Installer-verified:** 10.4 through 11.6 (12 versions, deep verification: kext + symbols + frameworks + PCI class)
- **Runtime-tested:** 10.11.6 El Capitan (PVE), 26.3 Tahoe (native)

### CI/CD
- **Multi-version test matrix:** macos-14, macos-15, macos-26
- **i386 build** via legacy MacOSX10.13.sdk download in CI
- Self-test validation (text + JSON) in CI pipeline
- ASAN smoke tests expanded to 15 commands
- 48 unit + 31 proactive + 210k fuzz + 63 integration tests

### Fixes
- LaunchDaemon plist: `--daemon` changed to `--daemonize` (primary flag name)
- Command count corrected to 45 across all docs
- Test count corrected to 63 across all docs
- Evidence terminology standardized: runtime-tested, PVE-integrated, installer-verified, best-effort
- All version claims made consistent (10.4+ not 10.7+)

## v2.2.0 (2026-03-23)

### Major Changes
- **Real filesystem freeze** — replaces fake no-op with actual freeze:
  - APFS (10.13+): atomic COW snapshot via `tmutil localsnapshot`
  - All versions: `sync()` + `F_FULLFSYNC` flushes data to physical media
  - Continuous `sync()` every 100ms during freeze window
  - Auto-thaw safety timeout (10 minutes)
  - Command filtering: only freeze-safe commands allowed during freeze
- **Freeze hook scripts** — `/etc/qemu/fsfreeze-hook.d/` (same model as Linux qemu-ga)
  - Scripts called with "freeze"/"thaw" argument
  - 30-second per-script timeout
  - Strict ownership validation (root-owned, not world-writable)

### Security Fixes
- Fixed password memory exposure (zero on all exit paths)
- Fixed command injection in diskutil calls (use execv, not shell)
- Fixed command injection in service update (use execv, not shell)
- Fixed unchecked `pipe()` in guest-exec (could use uninitialized fds)
- Fixed unchecked `fork()` in shutdown handler
- Fixed `WIFSIGNALED` called on extracted exit code instead of raw wait status
- Check all `mkdir()`, `chown()`, `tcsetattr()` return values
- Replace all `strtok()` with thread-safe `strtok_r()`

### Testing
- 48 unit tests + 31 proactive tests + 210,000 fuzz rounds + 62 integration tests
- Code coverage: 55.74% line, 80.27% function (remaining is untestable-in-CI code)
- Proactive tests: channel API, SSH key operations, hook validation, injection prevention

### Documentation
- Backup consistency section in README (freeze behavior, hook scripts, limitations)
- Thin disk provisioning guide (ssd=1, trimforce, TRIM, zero-fill reclaim)
- SECURITY.md updated with freeze hook security model

## v2.1.0 (2026-03-21)

### Major Changes
- **ISA serial transport** — uses Apple's built-in `Apple16X50Serial.kext` instead of VirtIO serial. No custom kernel extensions, no SIP issues, no code signing required. Works on macOS versions with the built-in Apple16X50Serial.kext driver.
- **PVE setup**: `qm set <vmid> --agent enabled=1,type=isa`

### Security
- Password changes via `dscl` now pipe password through stdin instead of command line arguments (no longer visible in `ps aux`)
- Passwords are zeroed in memory after use
- SECURITY.md documenting trust model and hardening options

### Fixes
- Serial port raw mode (no ICANON, no OPOST, no ECHO) for reliable bidirectional communication
- Buffer-check-before-poll: immediately process queued commands when PVE sends sync + command in one write
- Silently discard malformed messages to prevent stale data corruption in the serial buffer
- Removed O_NONBLOCK from serial port open (caused writes to not flush on macOS)

### Features
- `block-rpcs` and `allow-rpcs` fully implemented (were previously parsed but not enforced)
- Log rotation via newsyslog (5 files, 1MB max each)
- ISA serial device auto-detection (`/dev/cu.serial1`)
- Big Sur+ also works with default `type=virtio` via Apple's native VirtIO driver

### Removed
- VirtIO serial kernel extension (unnecessary with ISA serial)

## v2.0.0 (2026-03-21)

### Initial Release
- Native C implementation of the QEMU Guest Agent protocol
- 44 registered QGA commands (34 stable, 5 caveated, 1 no-op, 2 error, 2 aliases)
- Zero external dependencies (cJSON embedded)
- CLI flags compatible with Linux `qemu-ga`
- Configuration file compatible with `/etc/qemu/qemu-ga.conf`
- LaunchDaemon service with `--install` / `--uninstall`
- Binaries: i386 (10.4+), x86_64 (10.6+), arm64 (11.0+), universal
- Tested on macOS Tahoe 26.3 and Mac OS X El Capitan 10.11.6
