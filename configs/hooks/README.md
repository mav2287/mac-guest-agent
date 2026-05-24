# Freeze Hook Scripts

Drop-in scripts for `/etc/qemu/fsfreeze-hook.d/` that run during filesystem freeze and thaw operations. These ensure application-level consistency during PVE backups.

## Available Hooks

| Script | Application | On Freeze | On Thaw |
|---|---|---|---|
| `mysql.sh` | MySQL / MariaDB | FLUSH TABLES WITH READ LOCK | Release lock |
| `postgresql.sh` | PostgreSQL | CHECKPOINT | Nothing (auto-resumes) |
| `redis.sh` | Redis | BGSAVE + wait | Nothing (auto-resumes) |
| `launchd-service.sh` | Any launchd service | Stop services | Restart services |

## Installation

```bash
# Copy the hooks you need
sudo cp mysql.sh /etc/qemu/fsfreeze-hook.d/
sudo chmod 755 /etc/qemu/fsfreeze-hook.d/mysql.sh
sudo chown root:wheel /etc/qemu/fsfreeze-hook.d/mysql.sh
```

## Requirements

All hooks must:
- Be owned by root (uid 0)
- Not be world-writable (no `o+w`)
- Be executable (`chmod 755`)
- Complete within 30 seconds

The agent validates these before execution. Run `sudo mac-guest-agent --self-test` to verify.

## Execution Order

- **Freeze:** scripts run in alphabetical order
- **Thaw:** scripts run in reverse alphabetical order

Prefix with numbers to control order: `00-mysql.sh`, `10-redis.sh`, `99-launchd-service.sh`.

## Writing Custom Hooks

```bash
#!/bin/bash
case "$1" in
    freeze)
        # Called BEFORE filesystem freeze
        # Flush buffers, acquire locks, pause writes
        ;;
    thaw)
        # Called AFTER filesystem thaw
        # Release locks, resume writes
        ;;
esac
exit 0
```

**Exit-code contract:**

- **Freeze hooks:** a non-zero exit aborts the freeze. The agent returns a `GenericError` with description `Freeze hook script failed`, and `guest-fsfreeze-status` continues to report `thawed`. This is the safer default for backup consistency — a freeze hook is typically flushing in-flight writes (`FLUSH TABLES WITH READ LOCK`, `CHECKPOINT`, `BGSAVE`, etc.) and a failed flush means the snapshot would be inconsistent. If your hook should warn but not block the backup, log the warning and `exit 0` yourself.
- **Thaw hooks:** a non-zero exit only logs a warning; thaw still proceeds. Refusing to thaw on hook failure would leave the VM filesystem indefinitely frozen.

This contract is enforced by `src/cmd-fs.c run_hooks()` — see `docs/design/FREEZE_SEMANTICS.md` for the full freeze flow.
