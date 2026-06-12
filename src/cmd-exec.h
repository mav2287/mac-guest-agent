#ifndef MGA_CMD_EXEC_H
#define MGA_CMD_EXEC_H

/* Register guest-exec / guest-exec-status with the command table. */
void cmd_exec_init(void);

/* Drain all in-flight guest-exec processes' pipes (nonblocking) and
 * reap any that have exited via waitpid(WNOHANG). Called from the
 * agent's main poll loop on every wake-up tick (~1s) so a verbose
 * child's pipe doesn't back up while the caller is between
 * guest-exec-status polls. Cheap when nothing is in flight.
 *
 * See src/cmd-exec.c for the async design rationale (matches upstream
 * Linux / Windows qemu-ga's async model). */
void cmd_exec_drain_all(void);

/* 1 if any captured child still has an open stdout/stderr pipe (output may
 * still be arriving). The main loop polls fast while this is true so a verbose
 * child isn't throttled to ~64 KB/s by the slow idle tick. */
int  cmd_exec_has_pending_output(void);

#endif
