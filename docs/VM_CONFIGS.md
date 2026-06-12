# VM Configs & Troubleshooting — *is it the VM or the agent?*

On old macOS under QEMU the line between a **hypervisor/VM problem** and an
**agent bug** blurs fast. A wedged serial channel, a hung "sleep", a dropped
response — these look like agent faults but are almost always the emulated
platform. This doc is two things:

1. **Per-OS known-good VM configs** — the baseline that each macOS version wants.
2. **A symptom → cause → side → fix catalog** that tells you *which layer to look
   at first*, and points back at the config that fixes it.

These are generalized from the project's PVE/QEMU test fleet (Tiger 10.4 →
El Capitan 10.11, plus Apple-Silicon arm64). Deeper per-OS setup lives in
[`TIGER_ON_PVE.md`](TIGER_ON_PVE.md) and [`PVE.md`](PVE.md); the matching test
evidence is under [`evidence/v2.5.5/`](evidence/v2.5.5/).

---

## Per-OS reference configs (QEMU / Proxmox VE)

| Setting | Tiger 10.4 | Leopard 10.5 | Snow Leopard 10.6 | El Capitan 10.11 | Big Sur+ / arm64 |
|---|---|---|---|---|---|
| `machine` | `q35` | `q35` | `q35` | `q35` | (VZ / UTM) |
| **`cores`** | **≥ 2** | **≥ 2** | **≥ 2** | **≥ 2** | ≥ 2 |
| `cpu` | `Penryn` (kvm=off) | `Penryn` | `Penryn` | `host` / `Penryn` | host (VZ) |
| `memory` | 2 GB | 2 GB | 2 GB | 4–8 GB | 4 GB+ |
| Boot firmware | OVMF + OpenCore | OVMF + OpenCore | OVMF + OpenCore | OVMF + OpenCore | — |
| Installed slice | **i386** | i386 | x86_64 | x86_64 | arm64 |
| Disk bus | SATA/IDE | SATA/IDE | SATA/IDE | SATA | VirtIO |
| Agent transport | **ISA serial** | ISA serial | ISA serial | ISA serial | ISA serial |
| OpenCore `boot-args` | `keepsyms=1 -v idlehalt=1` | `keepsyms=1 -v` | `keepsyms=1 -v` | `-v keepsyms=1 -no_compat_check` | — |

**Non-obvious settings and *why*:**

- **`cores: 2` is load-bearing, not a nicety.** With a single vCPU the guest
  can't service the emulated 16550 UART receive interrupt fast enough under load;
  the RX FIFO overruns, a frame drops, and the QGA serial channel **wedges
  permanently** (dead until `qm reset`) while the OS keeps running fine. A second
  core drains the IRQ in parallel and the channel self-recovers. See
  [the wedge root-cause](#serial-channel-wedges-under-load). **Verified fix.**
- **No `cpus=1` in `boot-args`.** DarwinKVM-style cookbooks add `cpus=1` for
  old-macOS SMP stability — but it caps the kernel to one core, which *re-creates*
  the single-vCPU wedge no matter what `cores` says. OpenCore re-injects it every
  boot from `config.plist` (NVRAM Add+Delete), so it must be removed from
  `config.plist`, not via in-guest `nvram`. Tiger/Leopard/SL all run SMP stably
  with it removed (tested, no panic). Keep `idlehalt=1` on Tiger.
- **`machine: q35`** — all four old macOS boot and run on bare `q35` (Tiger
  included). The pinned older `pc-q35-6.x` types are *not* required; standardize
  on `q35`. (Note the **args gotcha** below — a raw `-machine type=…` in `args:`
  overrides the `machine:` field.)
- **ISA serial, not VirtIO** — the agent speaks over an ISA 16550, because old
  macOS has no VirtIO and Apple's own VirtIO agent isn't present pre-11.
  See [`PLATFORMS.md`](PLATFORMS.md).
- **Tiger installs the i386 slice** — a Tiger 10.4.7+ VM grades the x86_64 slice
  higher, but Tiger's `/bin` is i386/ppc only, so an x86_64 daemon hits
  `EBADEXEC` on every `execve` (this was issue #11). The agent's installer
  extracts the `uname -m` slice (i386 on Tiger). See
  [`evidence/v2.5.5/LIVE_MATRIX.md`](evidence/v2.5.5/LIVE_MATRIX.md).

### The `args:` vs `machine:` gotcha

Several lab VMs carry a raw `args: -machine type=q35 …` line **and** a
`machine: pc-q35-7.0` field. QEMU uses the **last** `-machine`, so the `args:`
one wins; the `machine:` field is inert. When auditing, check the **running**
process (`ps -ww -p $(cat /var/run/qemu-server/<id>.pid)`) for the effective
`-machine` / `-smp`, not just `qm config`.

---

## Is it the VM or the agent? — quick decision guide

| What you see | Look at the **VM** first if… | Look at the **agent** first if… |
|---|---|---|
| Channel dead, no reply | OS is fine at the console/desktop (serial transport wedged) | OS is up *and* a fresh channel also fails the same command |
| Command hangs | only under sustained/large traffic (UART/vCPU) | it hangs on the *first* clean call, idle VM |
| Wrong/empty data | only on one OS that grades a wrong arch slice | every OS returns the same wrong shape |
| "Sleep"/suspend hangs the VM | always — in-guest S3 has no QEMU wake (see below) | n/a — the agent only *issues* the sleep |
| Boot won't complete / Safe Boot | after a power-cycle (Hackintosh cold-boot flake) | never — agent isn't involved in boot |

Rule of thumb: **if the OS is alive at the console but the channel is dead, it's
the transport (VM), not the agent.** Grab a `screendump` from the QEMU monitor to
check.

---

## Troubleshooting catalog

### Serial channel wedges under load
- **Side:** VM (single vCPU + emulated 16550 RX-FIFO overrun).
- **Symptom:** `guest-ping` goes dead under sustained/bursty traffic; OS stays at
  a healthy desktop; only a `qm reset` brings the channel back.
- **Fix:** give the guest **≥ 2 cores** *and* remove `cpus=1` from the OpenCore
  `config.plist` boot-args. After that the channel survives thousands of rapid
  commands, 180 KB chunked I/O, and exec-spawn storms, and self-recovers from
  oversized-message bursts. Config: [`cores`](#per-os-reference-configs-qemu--proxmox-ve).
- **Evidence:** [`evidence/v2.5.5/os-level-destructive.md`](evidence/v2.5.5/os-level-destructive.md).

### Large inbound message loses bytes (~1.3 KB threshold)
- **Side:** VM (16550 RX FIFO; affects **every** config including 2-core El Cap).
- **Symptom:** a single QGA message bigger than ~1.3 KB (e.g. an un-chunked
  `guest-file-write`) loses bytes; that one exchange times out, then the channel
  recovers. *Not* a wedge.
- **Fix:** chunk inbound payloads ≤ ~900 B (the deploy tooling does). This is a
  hardware property below the agent — it does **not** go away with more cores.
- **Evidence:** [`evidence/UART_DRAIN.md`](evidence/UART_DRAIN.md).

### `guest-suspend-*` / Apple-menu Sleep hangs the VM
- **Side:** VM (broken S3 wake), *not* the agent or `pmset`.
- **Symptom:** `guest-suspend-disk`/`-ram`/`-hybrid` (or a user clicking Sleep)
  parks the VM with no resume. Verified identical with `osascript … to sleep`
  (no `pmset`), so it's the platform, not how the sleep is requested — both route
  through `IOPMSleepSystem → IOPMrootDomain → ACPI S3`, and these guests have no
  working wake-from-S3 path.
- **Fix (VM):** disable S3 so sleep is a harmless no-op — `-global
  ICH9-LPC.disable_s3=1` (the QEMU-macOS standard; El Cap effectively does this
  and "instant-wakes"). **Fix (host):** suspend at the hypervisor with
  `qm suspend` / `virsh suspend`, which freezes+resumes the whole VM.
- **Agent stance:** `guest-suspend-ram`/`-hybrid` are gated off and point to the
  host-side path; `guest-suspend-disk` is the open gating decision.
- **Evidence:** [`evidence/v2.5.5/os-level-destructive.md`](evidence/v2.5.5/os-level-destructive.md).

### `guest-exec` → `EBADEXEC`, or `network-get-interfaces` empty (Tiger only)
- **Side:** *was* the agent (x86_64 slice running on Tiger); fixed in v2.5.5.
- **Symptom:** on Tiger, exec of any system tool fails `EBADEXEC`; native
  getifaddrs/IOServiceMatching hang in the daemon.
- **Fix:** install the **i386** slice on Tiger (the agent installer now does this
  automatically). If you see it, confirm `uname -m` = `i386` and the installed
  binary is a non-fat i386 Mach-O.
- **Evidence:** [`evidence/v2.5.5/cleanup-relauncher-and-workarounds.md`](evidence/v2.5.5/cleanup-relauncher-and-workarounds.md).

### VM boots into Safe Boot / hangs at `AppleIntelCPUPowerManagement`
- **Side:** VM (Hackintosh cold-boot flake; intermittent).
- **Symptom:** after a `guest-shutdown` + restart, an old-macOS VM occasionally
  hangs mid-boot (vCPU idle) or lands in Safe Boot, where third-party
  LaunchDaemons (including the agent) don't auto-start.
- **Fix:** retry the boot — a clean one usually catches on the next attempt;
  otherwise `qm rollback` to a good snapshot. Not an agent issue (the agent isn't
  involved until launchd runs).

### OpenCore boot picker waits for a keypress after restart
- **Side:** VM (OpenCore `Misc/Boot/Timeout`).
- **Symptom:** after `guest-shutdown` + `qm start`, the VM sits at the OpenCore
  disk picker instead of auto-booting.
- **Fix:** set a `Timeout` in `config.plist`, or send a `sendkey ret` via the
  QEMU monitor. El Cap auto-boots; the older cookbook images don't.

### Hard-stop risk on a non-journaled volume
- **Side:** VM (filesystem state).
- **Symptom:** a guest whose HFS+ journal was cleared (e.g. for an offline
  rw-mount) risks corruption on `qm stop`/`reset`.
- **Fix:** re-enable journaling from inside the guest — `diskutil enableJournal /`
  — before relying on hard power cycles; snapshot first regardless.

---

## Apple-Silicon (arm64) note

The arm64 slice can't be tested under QEMU on an Apple-Silicon host (macOS only
virtualizes via Virtualization.framework there). Use an isolated **tart** VM
(`cirruslabs/macos-*`). The agent installs/runs identically (one universal
binary → thin arm64 slice); the host's QGA serial channel may be absent there,
so use `--test` mode for command coverage. See
[`evidence/v2.5.5/LIVE_MATRIX.md`](evidence/v2.5.5/LIVE_MATRIX.md).

---

*When you hit something new, add it here with its **side** (VM vs agent) and the
config that resolves it — that boundary is the single most useful thing this
project can document.*
