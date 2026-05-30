# VirtIO Override — When ISA Isn't an Option

> **This is an unsupported configuration.** The supported transport on macOS is ISA serial. Read this only if your orchestrator hardcodes VirtIO at the QGA-channel level and ISA truly isn't available on your host. If you have any way to expose an ISA UART to the guest, do that instead — see [`docs/PVE.md`](PVE.md), [`docs/LIBVIRT.md`](LIBVIRT.md), or [`docs/UTM.md`](UTM.md).

## Who this page is for

A small population:

- macOS 11 Big Sur and newer (the VirtIO console driver, `applevirtio.console`, didn't ship before that — earlier releases cannot use this override at all)
- Hosted under an orchestrator that auto-injects a `virtio` guest-agent channel with no way for the operator to switch the transport at the libvirt-domain level (kubevirt is the known case as of this writing; any orchestrator with the same constraint qualifies)
- Operator is willing to disable System Integrity Protection (SIP) and replace Apple's built-in `AppleQEMUGuestAgent` on this VM

If any of those don't apply, this page isn't for you.

## What this override does

| Step | Action |
|---|---|
| 1 | Confirm macOS ≥ 11, SIP off, `AppleQEMUGuestAgent` LaunchDaemon present, VirtIO guest-agent device file present |
| 2 | Print the full risk block and require an interactive `yes` to proceed |
| 3 | Unload Apple's `AppleQEMUGuestAgent` LaunchDaemon |
| 4 | Verify the unload landed (LaunchDaemon gone, no process holding the VirtIO device) |
| 5 | Install `mac-guest-agent` as usual + write `/etc/qemu/qemu-ga.conf` with `path = /dev/cu.org.qemu.guest_agent.0` |
| 6 | Start the LaunchDaemon and confirm the agent process is running and that `/var/log/mac-guest-agent.log` shows `Opened device: /dev/cu.org.qemu.guest_agent.0` |

If any step fails, no further changes are made and the installer reports the precise failure point. If the unload landed but the install or functional verify failed, the installer attempts to reload Apple's LaunchDaemon to restore the prior state.

## Prerequisites

### macOS 11 (Big Sur) or newer

`applevirtio.console` ships from macOS 11 onward. On 10.4 Tiger through 10.15 Catalina there is no Apple-supplied VirtIO console driver and `/dev/cu.org.qemu.guest_agent.0` does not exist. Those releases must use ISA serial — no override available, no engineering workaround on our side.

### SIP must be disabled

The `AppleQEMUGuestAgent` LaunchDaemon plist lives in `/System/Library/LaunchDaemons/`, which is SIP-protected. There is no Apple-supported way to disable a LaunchDaemon rooted in `/System/Library/` while SIP is enabled — every Apple-provided override mechanism (`launchctl unload -w`, `launchctl disable system/…`, override plist in `/Library/LaunchDaemons/`) ultimately writes to a SIP-protected path or is overridden by the system-domain plist.

To disable SIP:

1. Reboot the VM into Recovery (hold Command-R during boot, or via the hypervisor's NVRAM reset).
2. In Recovery, open Terminal and run:
   ```
   csrutil disable
   ```
3. Reboot back into macOS.
4. Verify with `csrutil status` — should report `System Integrity Protection status: disabled.`

**Security implication:** SIP off reduces the kernel-integrity posture of the VM. Code-signing enforcement on system binaries, restrictions on kext loading, and protection of system files all relax. This is a real reduction in security guarantees. Make sure the operating posture of the VM justifies it.

### A VirtIO guest-agent channel must be configured on the host

The override only works if your hypervisor is already exposing a virtio-serial port named `org.qemu.guest_agent.0` to the guest. The installer will refuse to proceed if `/dev/cu.org.qemu.guest_agent.0` doesn't exist on the guest.

For kubevirt, this is automatic — every VMI gets a `<channel type='unix'><target name='org.qemu.guest_agent.0' type='virtio'/></channel>` injected by the launcher. If yours doesn't have one, check the VMI's domain XML via `kubectl describe vmi <name>` or `virsh dumpxml <domain>` inside the virt-launcher pod.

## Running the override install

After SIP is off and the VM is back up:

```bash
# On the guest, after copying the binary to /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install --virtio
```

(Or, equivalently, via the bootstrap wrapper:  `sudo bash install.sh --virtio` — same end result; the wrapper just downloads the binary first.)

The binary will:

1. Run the prerequisite checks. Any failure prints a specific message and exits — no changes made.
2. Print the warning block. Read it. The risks listed there are real.
3. Prompt `Proceed? [yes/no]:` reading from `/dev/tty`. Type `yes` to continue, anything else (including no input) aborts. Cannot be bypassed with `yes | mac-guest-agent --install --virtio`.
4. Unload Apple's daemon and verify it released the channel.
5. Install `mac-guest-agent` as usual.
6. Write `/etc/qemu/qemu-ga.conf` with the explicit `path =` override.
7. Start the LaunchDaemon and confirm two signals — the agent's PID appears in `launchctl list com.macos.guest-agent`, AND the agent's log shows a fresh `Opened device: /dev/cu.org.qemu.guest_agent.0` line written after the restart. The check is local-state-only — it does not perform a host-to-guest QGA round-trip.

A marker file at `/var/db/mac-guest-agent/.virtio-mode` (content: `mode=full`) is dropped on success so `--uninstall` knows to restore Apple's daemon.

### DIY path (`--virtio-force`)

For operators who've already arranged for Apple's daemon to not own the channel (SIP off by hand, AppleQEMUGuestAgent unloaded by hand, possibly a non-standard device path) and want a one-line install without re-running the same checks:

```bash
sudo /usr/local/bin/mac-guest-agent --install --virtio-force
```

`--virtio-force` bypasses every prereq check, doesn't unload Apple's daemon, doesn't print the warning, doesn't prompt. It just writes the override config and drops the marker with `mode=force` so `--uninstall` knows NOT to touch Apple's daemon (since we didn't).

## Rolling back

```bash
sudo /usr/local/bin/mac-guest-agent --uninstall
```

The binary's `--uninstall` is marker-aware (v2.5.3+). It reads `/var/db/mac-guest-agent/.virtio-mode` and:

- Stops and removes the agent (LaunchDaemon plist + binary)
- Removes `/etc/qemu/qemu-ga.conf` (only when a marker is present — operator's pre-existing config is left alone)
- Reloads `/System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist` if marker mode is `full`
- Leaves AppleQEMUGuestAgent alone if marker mode is `force` (since we didn't unload it at install time)
- Removes the marker file

SIP is not re-enabled automatically — that's an operator action via Recovery + `csrutil enable`.

SIP is not re-enabled automatically. To restore SIP, reboot to Recovery and run `csrutil enable`.

## Stability promises

There are none for this configuration.

- This page documents a workaround for a host-side constraint we cannot fix on our side. It is not a supported transport.
- We do not run CI against this configuration. We do not collect Tier 1 evidence for it. Regressions specific to this configuration may land between releases without notice.
- macOS updates have the documented ability to re-enable system LaunchDaemons and reset SIP-protected state. After any macOS update on the VM, expect to re-verify this configuration and possibly re-apply it.
- If `AppleQEMUGuestAgent` changes behavior (adds the commands kubevirt expects, adopts a different IOKit match, etc.) in a future macOS release, this override may stop being necessary — but until that happens, no promise is being made about how it interacts.

If any of the above is unacceptable for your environment, the supported answer is ISA serial — every other docs page covers that path.

## What about driver-level takeover?

A System Extension (DriverKit) could in principle compete for the IOKit match and claim the VirtIO console device before Apple's `applevirtio.console` does, removing the SIP-off requirement. We have evaluated this and explicitly chosen not to ship it. The cost is months of engineering, a paid Apple Developer Program enrollment, notarization on every release, a user-approval gesture at install time, and a perpetual maintenance commitment against IOKit-match changes in every macOS release — for a population this page already describes as "small." The honest answer for this audience is the gated `--virtio` install path on this page, not a driver fight with Apple at the kernel boundary.
