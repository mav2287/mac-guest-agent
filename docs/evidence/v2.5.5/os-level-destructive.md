# v2.5.5 OS-level destructive tests (snapshot-gated)

Real, side-effecting lifecycle commands run **live against each daemon over the
QGA channel** (not `--test`, which dry-runs fsfreeze and would never really halt
the VM). Each VM was snapshotted first (`pre_os_destruct`, 2026-06-12) so any
wedge is recoverable by `qm rollback`.

Channel discipline (per the Tiger serial-wedge lessons): every command uses the
**request → hold-socket → single read** pattern, and any exec uses **one
guest-exec writing to a file, fixed wait, then guest-file-read** — never a tight
`guest-exec-status` poll loop.

## 1. fsfreeze / thaw — CLEAN on all three

macOS has no `FIFREEZE`; `cmd-fs.c` implements freeze as `sync()` +
`F_FULLFSYNC` across writable local mounts, holds a frozen flag with a 100 ms
re-sync loop, and arms a `SIGALRM` auto-thaw. It cannot block the VM.

| VM | freeze (→ #vols) | status after freeze | thaw (→ #vols) | status after thaw | ping |
|---|---|---|---|---|---|
| 112 Leopard | `2` | `frozen` | `2` | `thawed` | alive |
| 113 Snow Leopard | `2` | `frozen` | `2` | `thawed` | alive |
| 107 El Capitan | `1` | `frozen` | `1` | `thawed` | alive |

Daemon stayed responsive (`guest-ping` → `{}`) the whole time; the FS returned
to `thawed` on every box. The return value is the QGA-spec count of frozen
filesystems.

## 2. guest-shutdown (powerdown) + boot-back

`handle_shutdown` forks and the child runs `/sbin/shutdown -h now`.

**Shutdown half — clean on all three.** Every VM ACPI-powered-off within ~5 s
(`qm status` → `stopped`); next boot logged `Previous Shutdown Cause: 5` (the
macOS code for a *normal* shutdown). The agent command itself is correct and
proven on all three.

**Boot-back half — one clean, two blocked by guest-environment quirks (not the
agent):**

| VM | Boot-back outcome |
|---|---|
| 107 El Capitan | Auto-booted, agent came back up over QGA — **full clean boot-back**. |
| 113 Snow Leopard | Booted, but into **Safe Boot** (red banner in the login window) → third-party LaunchDaemons suppressed → agent did not auto-load. Recovered with `qm rollback pre_os_destruct` → clean boot → agent up. |
| 112 Leopard | First boot **hung** at the `waitForService(AppleIntelCPUPowerManagement)` stage (vCPU idle, 14 s CPU in 6 min); `qm reset` → **Safe Boot**. Recovered with `qm rollback` → clean verbose boot (journal replayed) → agent up on the first ping. |

Two guest-environment facts surfaced, both independent of the agent:
- These OpenCore VMs stop at the **boot picker** after a power-cycle (no
  auto-timeout) and need a `sendkey ret` to proceed. El Cap is configured to
  auto-boot; Leopard/SL are not.
- Leopard and Snow Leopard land in **Safe Boot** after the shutdown→restart
  cycle (El Cap does not). Safe Boot suppresses `/Library/LaunchDaemons`, so the
  agent won't start until a normal boot. This is an OpenCore/Hackintosh boot
  behavior; `guest-shutdown` did its job (clean ACPI poweroff, cause 5).

## 3. guest-suspend-disk

`do_suspend` saves `hibernatemode`, sets it to `25` (hibernate-to-disk), runs
`pmset sleepnow`, and restores `hibernatemode` on wake. suspend-ram (0) and
suspend-hybrid (3) are **gated off** precisely because parking the vCPU wedges a
VM with no resume path.

| Target | Result |
|---|---|
| 107 El Capitan | `{"return":{}}`; QEMU did an instant sleep/wake — VM stayed `running`, agent answered `guest-ping`, **`hibernatemode` restored to 0** (no side effect). Clean. |
| arm64 tart (VZ) | `GenericError: "Failed to initiate sleep"` — headless Virtualization.framework has no `pmset sleepnow`; the handler **fails secure** (no side effect, VM never slept). |
| 112 Leopard | **Actually suspended** (hibernatemode-25 + sleepnow genuinely slept the guest) and the VM **did not resume** — `system_wakeup` + key nudge had no effect (this OpenCore/QEMU config has no working S3/S4 resume). Recovered with `qm rollback`. |

### Root cause — WHY it wedges (and it is not the agent)

`pmset sleepnow`, the **Apple-menu → Sleep** item, and `guest-suspend-disk` all
funnel into the **same** kernel transition: a userspace sleep request →
`IOPMSleepSystem()` → `IOPMrootDomain` → ACPI **S3** entry (per Apple's XNU
docs, userspace `IOPMSleepSystem()` calls travel through the `IOPMrootDomain`
path). The only thing `do_suspend` adds is forcing `hibernatemode` to 25 first;
the sleep *entry* is identical to a user clicking Sleep.

S3 = suspend-to-RAM: the kernel powers devices down and halts the vCPU, then
waits for a **hardware wake event** to resume. On these OpenCore/QEMU guests
there is **no working wake-from-S3 path** (the #1 Hackintosh sleep failure —
PCIe/USB devices don't cooperate on resume, no usable ACPI wake/RTC). So macOS
sleeps and never comes back → wedge. `system_wakeup` from the QEMU monitor did
not revive it (the guest's resume path is what's broken).

**Empirical proof it is the VM, not the agent:** firing the Apple-menu Sleep
equivalent on SL 113 — `osascript -e 'tell application "System Events" to
sleep'`, **no `pmset`, hibernatemode left at 0** — wedged the VM identically
(status `running`, vCPU idle, agent gone). A human clicking Sleep hangs these
guests exactly as the agent does.

**Why El Capitan didn't wedge:** its effective machine/ACPI does an *instant
wake* (the sleep request returns without a real S3 park) — equivalent to having
S3 disabled. Leopard/SL (`pc-q35-6.1+pve0`, older ACPI) actually enter S3 and
wedge.

### Real fixes (two layers)

- **VM layer (system-wide, fixes menu-Sleep too):** disable S3 at the
  hypervisor — `-global ICH9-LPC.disable_s3=1` (documented QEMU-macOS fix; used
  by quickemu). Sleep then becomes a harmless no-op/instant-return like El Cap,
  so neither a user nor the agent can wedge the guest. Alternatively, fix wake
  (ACPI SSDT/AppleRTC/USB-map) — impractical on these legacy OSes.
- **Agent layer (so the agent is never a wedge vector):** in-guest suspend is
  the wrong layer for a hypervisor-managed VM — the reliable suspend is host-side
  (`qm suspend` / `virsh suspend`), which freezes and resumes the whole VM at the
  hypervisor and needs no guest support. Standard QGA has no native macOS
  suspend-to-disk anyway (returns `Unsupported`). So gate `guest-suspend-disk`
  like ram/hybrid and point to the host-side path.

### Finding: suspend-disk is not "safe/resumable" under these QEMU guests

`cmd-power.c` keeps `guest-suspend-disk` enabled on the rationale that
hibernate-to-disk is "like a resumable shutdown and is safe," while gating
suspend-ram/hybrid. On **real Mac hardware** hibernatemode-25 does hibernate and
resume normally. But on the **Leopard QEMU** guest it slept and never came back
— the same no-resume wedge that justifies gating ram/hybrid. El Cap survived
only because its QEMU config instant-wakes instead of truly sleeping.

This is a guest-environment limitation, not an agent logic bug (the agent issued
the correct `pmset` sequence and restored state on the paths that returned). But
it means **suspend of any kind cannot be relied on in these QEMU macOS VMs**;
only `guest-shutdown` is a dependable "stop the guest" primitive here. Worth a
follow-up decision: either gate `guest-suspend-disk` on the same older-macOS /
VM-without-resume condition as ram/hybrid, or document the resume requirement.
Not changed in code unilaterally — it is a real, working feature on physical
hardware. (Surfaced to the operator.)

## Serial-channel wedge — ROOT CAUSE FOUND: `cpus=1` (single vCPU), not the agent

The Tiger/Leopard/SL QGA channel "wedges under load" — guest-ping goes dead and
the VM needs a reset, while the OS stays at a healthy desktop. Root-caused on
Leopard VM 112 with an A/B at the hypervisor layer.

**The differentiator is vCPU count, forced by an OpenCore boot-arg.** El Cap
(107) runs 2 cores and never wedges; 111/112/113 are effectively single-core.
The cause is not the OS version, the machine type, or the `-smp` topology — it
is an explicit **`cpus=1`** kernel boot-arg baked into the OpenCore
`config.plist` (`NVRAM → Add → boot-args = "keepsyms=1 -v cpus=1"`), a DarwinKVM
cookbook default for old-macOS SMP stability. OpenCore *re-injects* it every boot
(it's in the NVRAM Delete+Add set), so an in-guest `nvram boot-args=...` change
is overwritten — it must be edited in `config.plist`.

Evidence chain (all on 112):
- `-smp 2,cores=2` and `-smp 2,sockets=2` and bare `q35` machine: guest still
  `hw.ncpu = 1`. ACPI presents the CPUs (ioreg shows multiple AppleACPICPU) but
  `cpus=1` caps the kernel to one.
- Edited `oc-leopard-boot.img:/EFI/OC/config.plist`, `keepsyms=1 -v cpus=1` →
  `keepsyms=1 -v`. After reboot: `hw.ncpu = 2`, `guest-get-vcpus = 2`, **Leopard
  SMP-stable** (clean boot, no panic).

**Wedge behavior, A/B (identical single-socket burst load generator):**

| | 1 core (cpus=1) | 2 cores (cpus=1 removed) |
|---|---|---|
| 16 KB burst (8×2 KB) | channel **dead**, needs VM reset | survives, ping OK |
| 384 KB hammer | (never gets here) | survives, ping OK after each |
| 300 rapid pings | wedges | **300/300 ALIVE** |
| 700 B write/read round-trip | — | **byte-exact integrity** |

Mechanism: the emulated 16550 UART has a 16-byte RX FIFO; a single core under
load can't service the receive IRQ fast enough → overrun. On one core a dropped
frame desyncs the channel **permanently** (wedge). A second core services the
serial IRQ in parallel, so the channel **self-recovers** instead of dying.

**Residual (unchanged, non-fatal):** a single inbound message larger than
~1.3 KB still loses bytes (per-message FIFO overrun) — that message times out,
but the channel recovers. Handled by sender-side chunking (≤~900 B payload).

**Fix (minimal):** remove `cpus=1` from each guest's OpenCore `config.plist` and
set `cores: 2` — no machine-type change needed (verified on the original
`pc-q35-6.1` machine). Per-OS SMP-stability must be confirmed; **Tiger (10.4) is
the risk** (`cpus=1` may be load-bearing there — task #172 stabilization). The
agent needs no change; this is a VM-config fix that removes the wedge entirely.

## Applied fleet-wide — all VMs standardized to q35 + 2 cores

The wedge fix (drop `cpus=1` from OpenCore `config.plist` + `cores: 2`) plus a
machine-type standardization to bare `q35` was applied to the whole fleet and
verified. Every old macOS — **including Tiger 10.4** — boots stable on `q35` and
runs SMP without panic:

| VM | OS | machine | hw.ncpu | post-fix wedge test |
|---|---|---|---|---|
| 111 | Tiger 10.4.11 | q35 | 2 | 300/300 rapid pings ALIVE, 700 B write byte-exact |
| 112 | Leopard 10.5.8 | q35 | 2 | 300/300 + integrity (earlier) |
| 113 | Snow Leopard 10.6.8 | q35 | 2 | up, ncpu=2 |
| 107 | El Capitan 10.11 | q35 | 2 | baseline (never wedged) |

Tiger's boot-args were `keepsyms=1 -v cpus=1 idlehalt=1` — only `cpus=1` was
removed (`idlehalt=1` preserved). Tiger was journaled again first
(`diskutil enableJournal /`) so hard-cycles are safe. Config.plist images backed
up (`oc-*-boot*.img.bak-precpus`); safety snapshots `pre_2core` (112/113) and
`pre_q35_test` (111) retained until cleanup is confirmed.

## Recovery / housekeeping

All three VMs were restored to a healthy, agent-running state via
`qm rollback pre_os_destruct` (112, 113) or normal recovery (107). The
`pre_os_destruct` snapshots are retained until the operator confirms cleanup.
