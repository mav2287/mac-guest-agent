# Security

## Trust Model

The QEMU Guest Agent protocol assumes the **hypervisor is trusted**. Any entity that can write to the guest agent's serial port can execute commands inside the VM, including:

- Arbitrary command execution (`guest-exec`)
- Password changes (`guest-set-user-password`)
- SSH key injection (`guest-ssh-add-authorized-keys`)
- System shutdown/reboot (`guest-shutdown`)
- File read/write (`guest-file-*`)

This is by design — the hypervisor administrator needs these capabilities to manage VMs. This is identical to the Linux `qemu-ga` security model.

## Privilege Requirements

Commands that modify system state require the agent to run as root. The agent enforces this at startup (except in `--test` mode).

| Privilege | Commands |
|---|---|
| **No root needed** | `guest-ping`, `guest-sync`, `guest-sync-delimited`, `guest-info`, `guest-get-osinfo`, `guest-get-host-name`, `guest-get-hostname`, `guest-get-timezone`, `guest-get-time`, `guest-get-users`, `guest-get-load`, `guest-get-vcpus`, `guest-get-memory-blocks`, `guest-get-memory-block-info`, `guest-get-cpustats`, `guest-get-disks`, `guest-get-fsinfo`, `guest-get-diskstats`, `guest-fsfreeze-status`, `guest-network-get-interfaces`, `guest-file-close`, `guest-file-read`, `guest-file-write`, `guest-file-seek`, `guest-file-flush`, `guest-exec-status` |
| **Root required** | `guest-set-time`, `guest-shutdown`, `guest-fsfreeze-freeze`, `guest-fsfreeze-freeze-list`, `guest-fsfreeze-thaw`, `guest-file-open`, `guest-exec`, `guest-ssh-get-authorized-keys`, `guest-ssh-add-authorized-keys`, `guest-ssh-remove-authorized-keys`, `guest-set-user-password` |
| **Disabled** (`enabled=0` → `CommandNotFound`; opt-in via `allow-rpcs`) | `guest-suspend-disk`, `guest-suspend-ram`, `guest-suspend-hybrid` — no working wake path on QEMU/OpenCore guests; use host-side `qm suspend`/`virsh suspend` |
| **Not registered** (absent from `guest-info`; `CommandNotFound`) | `guest-fstrim`, `guest-set-vcpus`, `guest-set-memory-blocks` — no equivalent macOS API; matches upstream QEMU `CONFIG_*` gating |

See [docs/COMMAND_STATUS.md](docs/COMMAND_STATUS.md) for full per-command details.

## Recommended Profiles

Use `--allow-rpcs` (allowlist mode) to restrict the agent to only the commands needed for your use case. When `--allow-rpcs` is set, all commands not in the list are blocked.

### Minimal (monitoring only)

Read-only status queries. No execution, no file access, no modifications.

```ini
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-network-get-interfaces
```

### PVE Management (standard Proxmox operations)

Supports shutdown, freeze/thaw for backups, and system info queries. No exec, no file I/O, no SSH key management, no password changes.

```ini
[general]
allow-rpcs = guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-set-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-disks,guest-get-fsinfo,guest-get-diskstats,guest-fsfreeze-status,guest-fsfreeze-freeze,guest-fsfreeze-thaw,guest-network-get-interfaces,guest-shutdown
```

### Full Admin (all capabilities)

All *callable* commands enabled. This is the default when no `--allow-rpcs` or `--block-rpcs` is set. Equivalent to the Linux `qemu-ga` default — except the commands macOS can't honor are never callable regardless: `guest-fstrim`, `guest-set-vcpus`, `guest-set-memory-blocks` are not registered, and `guest-suspend-disk`/`-ram`/`-hybrid` are registered but disabled by default (opt-in via `allow-rpcs` only on a guest with a proven wake path). All other commands are enabled.

If you want full admin but want to block specific dangerous commands:

```ini
[general]
block-rpcs = guest-exec,guest-set-user-password
```

## Freeze-State Command Restrictions

While the filesystem is frozen (`guest-fsfreeze-freeze` has been called but `guest-fsfreeze-thaw` has not), the agent restricts which commands can execute. This prevents new disk writes from invalidating the freeze consistency guarantee.

**Allowed during freeze** (matches `fsfreeze_is_allowlisted()` in `src/cmd-fs.c`):
- `guest-ping`
- `guest-sync`, `guest-sync-delimited`
- `guest-sync-id` (extension — read-only, returns a caller-supplied correlation token)
- `guest-info`
- `guest-fsfreeze-status`
- `guest-fsfreeze-freeze` (idempotent — returns current frozen count)
- `guest-fsfreeze-freeze-list` (idempotent subset re-freeze — divergence from upstream)
- `guest-fsfreeze-thaw`

**Blocked during freeze:** all other commands. The agent returns a `GenericError` with a message indicating the filesystem is frozen.

Auto-thaw fires after 10 minutes if PVE never sends thaw (safety net against orphaned freeze state).

## Freeze Hook Script Security

Hook scripts in `/etc/qemu/fsfreeze-hook.d/` execute as root during freeze and thaw. The agent validates each script before execution:

- **Must be owned by root** (uid 0)
- **Must not be world-writable** (no `o+w` permission)
- **Must be executable** (has `x` permission for owner)
- **30-second timeout** per script — a script that exceeds it is killed with SIGKILL (a hung hook during a freeze must not extend the freeze window, so there is no SIGTERM grace period)
- **Execution order:** scripts run in alphabetical order on freeze, reverse order on thaw

Scripts that **fail validation** (wrong owner, world-writable, not executable) are skipped with a warning logged — these are configuration errors, not runtime failures, and the freeze proceeds without that hook.

A **freeze hook that runs but exits non-zero aborts the freeze.** The agent returns a `GenericError` with description `Freeze hook script failed`; the freeze does not complete, and `guest-fsfreeze-status` will continue to report `thawed`. This is the safer default for backup consistency: a freeze hook's purpose is typically to flush in-flight writes (`FLUSH TABLES WITH READ LOCK`, `CHECKPOINT`, `BGSAVE`, etc.), and a failed flush means the resulting snapshot would be inconsistent for that workload — better to fail the backup loudly than to ship a bad one quietly. Hook scripts that want to be advisory (warnings rather than aborts) should `exit 0` after logging the warning themselves.

Thaw hooks always run — a thaw hook returning non-zero only logs a warning, since refusing to thaw on hook failure would leave the VM filesystem indefinitely frozen.

Run `--self-test` to validate your hook scripts:

```bash
sudo mac-guest-agent --self-test
```

## Password Handling

`guest-set-user-password` passes the password to `dscl` via stdin pipe, not on the command line. The password buffer is zeroed in memory after use. The password is never logged.

## Command Injection Prevention

- Disk name inputs are validated against `[a-zA-Z0-9_-]` before use
- `diskutil` commands use `run_command_v()` (explicit argv) instead of shell interpolation
- Password changes use stdin pipe to `dscl`, not command-line arguments
- All `popen()`/`system()` calls use static command strings or validated inputs

## Reporting Vulnerabilities

Report security issues via GitHub Issues at https://github.com/mav2287/mac-guest-agent/issues with the `security` label, or contact the maintainers directly.
