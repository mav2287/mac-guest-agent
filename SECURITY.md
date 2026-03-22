# Security

## Trust Model

The QEMU Guest Agent protocol assumes the **hypervisor is trusted**. Any entity that can write to the guest agent's serial port can execute commands inside the VM, including:

- Arbitrary command execution (`guest-exec`)
- Password changes (`guest-set-user-password`)
- SSH key injection (`guest-ssh-add-authorized-keys`)
- System shutdown/reboot (`guest-shutdown`)
- File read/write (`guest-file-*`)

This is by design — the hypervisor administrator needs these capabilities to manage VMs. This is identical to the Linux `qemu-ga` security model.

## Hardening

To restrict which commands the agent accepts, use `block-rpcs` or `allow-rpcs`:

```bash
# Block dangerous commands
mac-guest-agent -b "guest-exec,guest-set-user-password,guest-ssh-add-authorized-keys,guest-ssh-remove-authorized-keys"

# Or allowlist only safe read-only commands
mac-guest-agent -a "guest-ping,guest-sync,guest-sync-delimited,guest-info,guest-get-osinfo,guest-get-host-name,guest-get-timezone,guest-get-time,guest-get-users,guest-get-load,guest-get-vcpus,guest-get-memory-blocks,guest-get-memory-block-info,guest-get-cpustats,guest-get-fsinfo,guest-network-get-interfaces,guest-fsfreeze-status"
```

Or in `/etc/qemu/qemu-ga.conf`:

```ini
[general]
block-rpcs = guest-exec,guest-set-user-password,guest-ssh-add-authorized-keys,guest-ssh-remove-authorized-keys
```

## Password Handling

`guest-set-user-password` passes the password to `dscl` via stdin pipe, not on the command line. The password is zeroed in memory after use.

## Reporting Vulnerabilities

Report security issues via GitHub Issues at https://github.com/mav2287/mac-guest-agent/issues with the `security` label, or contact the maintainers directly.
