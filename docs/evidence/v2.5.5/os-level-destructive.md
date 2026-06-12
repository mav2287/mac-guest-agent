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

## Recovery / housekeeping

All three VMs were restored to a healthy, agent-running state via
`qm rollback pre_os_destruct` (112, 113) or normal recovery (107). The
`pre_os_destruct` snapshots are retained until the operator confirms cleanup.
