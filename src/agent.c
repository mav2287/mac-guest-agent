#include "agent.h"
#include "channel.h"
#include "commands.h"
#include "cmd-fs.h"
#include "cmd-exec.h"
#include "protocol.h"
#include "log.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* SIGUSR1 status-dump request flag — set by the signal handler in main.c,
 * cleared by us once we've serviced it. Defined extern so we don't have
 * to thread an opaque "siginfo" pointer through agent_create/run. */
extern volatile sig_atomic_t g_dump_status_requested;

#define FREEZE_POLL_TIMEOUT_MS 100

/* Idle select() timeout for the read loop. Kept at 1000 ms. A short timeout was
 * tested (50 ms) to fight Tiger's inbound message loss by running the read-first
 * probe more often, but measurement showed it did NOT raise the loss threshold
 * (the bytes are dropped at the emulated-16550 RX overrun, below the agent — see
 * docs/evidence/UART_DRAIN.md), so the extra idle wakeups buy nothing. */
#define IDLE_POLL_TIMEOUT_MS 1000

/* Poll timeout while a guest-exec child still has captured output flowing.
 * Short (20 ms) so the per-tick cmd_exec_drain_all() empties the child's pipe
 * ~50x/s — lifts large-output capture from ~64 KB/s (one 64 KB pipe per idle
 * tick) to multi-MB/s. Only in effect while output is pending, so no idle cost. */
#define EXEC_DRAIN_POLL_TIMEOUT_MS 20

/* Stale-channel watchdog (issue #10).
 *
 * On Tiger 10.4 the BSD serial driver can wedge in a state where the fd
 * remains valid and select() keeps being called, but select() keeps
 * returning timeout (0) — the `selrecord` readiness wakeup never fires
 * even though PVE has continued to write to the host side of the pipe.
 * Empirically (per the v2.5.4 SIGUSR1 dumps captured during repro) the
 * agent loop is NOT stuck inside a syscall: select.calls advance at
 * exactly 1 Hz throughout. read() was never called during the wedge
 * window — we don't reach the read() path when select() reports nothing
 * ready — so we cannot yet say whether bytes are sitting in the tty
 * input queue invisible to selrecord, or whether they aren't even being
 * delivered to the queue.
 *
 * vit9696 reproduced this at ~4 hours of uptime: process alive, 0% CPU,
 * sample shows 300/300 frames in `select`, while `qm agent <vmid> ping`
 * from the host reports "QEMU guest agent is not running". Issue #2
 * already documented a related class of Tiger serial bugs (poll() returns
 * POLLNVAL for valid fds), so this driver is known fragile.
 *
 * Mitigation 1: read-first probe before each select() call (in
 * channel.c::channel_try_read). If the tty input queue has data that
 * selrecord has stopped reporting, the probe consumes it directly and
 * the wedge becomes a non-event. Probe-hit accounting (probe.hits
 * counter) tells us empirically whether this is sufficient.
 *
 * Mitigation 2 (this watchdog, kept as last-resort safety net): track
 * wall-clock time of the last successful message read, and if no
 * complete message has arrived in WATCHDOG_IDLE_TIMEOUT_SEC, log a
 * warning and force-cycle the channel (close + reopen). The reopen
 * resets the driver's per-fd state and re-establishes the pipe.
 * Reconnects are cheap (~50 ms on serial) and idempotent — even on a
 * healthy quiet agent (where PVE genuinely isn't polling) the periodic
 * reconnect is harmless.
 *
 * 600 s (10 min) is chosen as: long enough that a normal PVE setup
 * (which polls every 30 s) won't trigger spurious reconnects, short
 * enough that recovery from a wedged driver completes well within a
 * single monitoring window. Tunable via config in a future revision. */
#define WATCHDOG_IDLE_TIMEOUT_SEC 600

struct agent {
    channel_t *channel;
    volatile int running;
    int        frozen;
    int        test_mode;
};

agent_t *agent_create(const char *device_path, int test_mode)
{
    agent_t *ag = calloc(1, sizeof(*ag));
    if (!ag) return NULL;

    if (test_mode) {
        ag->channel = channel_create_test();
    } else {
        ag->channel = channel_create(device_path);
    }

    if (!ag->channel) {
        free(ag);
        return NULL;
    }

    ag->test_mode = test_mode;
    return ag;
}

static void process_message(agent_t *ag, const char *msg)
{
    cJSON *request = protocol_parse_request(msg);
    if (!request) {
        /* Per QMP spec: return an error for malformed JSON.
         * The client handles stale data via guest-sync / 0xFF delimiter. */
        LOG_DEBUG("Parse error: %.40s%s", msg, strlen(msg) > 40 ? "..." : "");
        char *resp = protocol_build_error("GenericError", "JSON parse error", NULL);
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        return;
    }

    const char *cmd_name = protocol_get_command(request);
    cJSON *args = protocol_get_arguments(request);
    const cJSON *id = protocol_get_id(request);

    if (!cmd_name) {
        /* Valid JSON but no "execute" field */
        char *resp = protocol_build_error("GenericError",
            "Missing 'execute' field", protocol_get_id(request));
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        cJSON_Delete(request);
        return;
    }

    /* During freeze, only allow freeze-safe commands. The gate applies only to
     * commands that actually exist — an unknown command (e.g. guest-fstrim,
     * which is not registered on macOS) must fall through to commands_dispatch()
     * and get CommandNotFound, not the misleading "not allowed while frozen". */
    if (command_exists(cmd_name) && !fsfreeze_command_allowed(cmd_name)) {
        char *resp = protocol_build_error("GenericError",
            "Command not allowed while filesystem is frozen", id);
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        cJSON_Delete(request);
        return;
    }

    int use_delimiter = (strcmp(cmd_name, "guest-sync-delimited") == 0);

    /* Flush stale OUTPUT before writing the sync response.
     * Previous PVE sessions may have disconnected before reading all
     * our responses, leaving stale data in the serial output buffer.
     * This clears pending output only — does NOT touch the input
     * buffer where the ping command is waiting. */
    if (use_delimiter) {
        channel_flush_stale_output(ag->channel);
    }

    char *resp = commands_dispatch(cmd_name, args, id);
    if (resp) {
        if (use_delimiter) {
            channel_send_delimited_response(ag->channel, resp);
        } else {
            channel_send_response(ag->channel, resp);
        }
        free(resp);
    }

    cJSON_Delete(request);
}

int agent_run(agent_t *ag, volatile sig_atomic_t *stop_flag)
{
    if (!ag) return -1;

    if (channel_open(ag->channel) != 0) {
        LOG_ERROR("Failed to open channel");
        return -1;
    }

    ag->running = 1;
    LOG_INFO("Agent started, listening for commands...");

    /* Watchdog state — last wall-clock time we successfully read a
     * message from the channel. Initialized to now so a freshly-opened
     * channel gets the full WATCHDOG_IDLE_TIMEOUT_SEC grace period. */
    time_t last_msg_time = time(NULL);

    while (ag->running && !(stop_flag && *stop_flag)) {
        /* v2.5.4 diagnostic: if SIGUSR1 set the flag, dump channel counters
         * from main-loop context (signal-safe). Defined in main.c. */
        if (g_dump_status_requested) {
            g_dump_status_requested = 0;
            channel_dump_status(ag->channel);
        }
        /* During freeze: shorten poll timeout and run continuous sync.
         * While a guest-exec child is still producing captured output, also
         * poll fast so we drain its pipe ~50x/s instead of once per idle tick
         * — otherwise the 64 KB pipe fills, the child blocks, and capture is
         * throttled to ~64 KB/s. The fast path costs nothing when no exec is
         * in flight (cmd_exec_has_pending_output() returns 0). */
        if (fsfreeze_is_frozen()) {
            channel_set_poll_timeout(ag->channel, FREEZE_POLL_TIMEOUT_MS);
        } else if (cmd_exec_has_pending_output()) {
            channel_set_poll_timeout(ag->channel, EXEC_DRAIN_POLL_TIMEOUT_MS);
        } else {
            /* IDLE_POLL_TIMEOUT_MS (was 1000). On Tiger select() frequently
             * won't wake on serial-data arrival (issue #10), so the per-tick
             * read-first probe is the only thing that pulls bytes — and it runs
             * once per this timeout. At 1 Hz the small (~2 KB) macOS tty input
             * queue overflows on a multi-KB inbound message before the agent
             * ever reads it (measured: loss above ~1.3 KB). A short timeout runs
             * the probe ~frequently enough to drain the queue before it
             * overflows. Cost is ~20 idle wakeups/s — negligible with the HLT
             * idle path; modern hosts also benefit (snappier dispatch). */
            channel_set_poll_timeout(ag->channel, IDLE_POLL_TIMEOUT_MS);
        }

        /* Drain in-flight guest-exec processes on every tick so a
         * verbose child's pipe doesn't back up while the caller is
         * between guest-exec-status polls. Cheap when nothing is in
         * flight (single nonblocking read returning EAGAIN per fd). */
        cmd_exec_drain_all();

        char *msg = channel_read_message(ag->channel);
        if (!msg) {
            if (errno == EAGAIN) {
                /* Normal timeout — run continuous sync if frozen */
                if (fsfreeze_is_frozen()) {
                    fsfreeze_continuous_sync();
                }
                /* Idle-channel watchdog: Tiger's serial driver can wedge
                 * after extended uptime such that select() perpetually
                 * times out even though the host-side writer is still
                 * sending. If we go too long with zero reads, the driver
                 * is presumed stuck — force a reconnect to reset its
                 * per-fd state. See WATCHDOG_IDLE_TIMEOUT_SEC comment
                 * at file top for rationale. */
                if (!ag->test_mode &&
                    (time(NULL) - last_msg_time) >= WATCHDOG_IDLE_TIMEOUT_SEC) {
                    LOG_INFO("No message received in %d seconds — cycling channel "
                             "(defense-in-depth; harmless on idle systems)",
                             WATCHDOG_IDLE_TIMEOUT_SEC);
                    /* Dump pre-reconnect counters for postmortem (helpful
                     * when correlating wedge timing across many reconnects). */
                    channel_dump_status(ag->channel);
                    channel_note_reconnect(ag->channel);
                    channel_close(ag->channel);
                    if (channel_open(ag->channel) != 0) {
                        LOG_ERROR("Watchdog reconnect failed; will retry on next tick");
                        sleep(5);
                    } else {
                        LOG_INFO("Watchdog reconnect succeeded");
                    }
                    last_msg_time = time(NULL);
                }
                continue;
            }
            if (errno == 0 && ag->test_mode) {
                /* stdin closed or user typed quit */
                LOG_INFO("Test session ended");
                break;
            }
            if (errno == EIO) {
                LOG_ERROR("Device connection lost, attempting reconnect...");
                channel_note_eio_reconnect(ag->channel);
                channel_close(ag->channel);
                sleep(5);
                if (channel_open(ag->channel) != 0) {
                    LOG_ERROR("Reconnect failed");
                    sleep(5);
                }
                /* v2.5.4 — DO NOT reset last_msg_time here.
                 *
                 * Resetting it on every EIO reconnect meant the 600 s idle
                 * watchdog could never fire during an EIO-storm wedge —
                 * the watchdog timer was reset every ~10 s as we cycled
                 * through close+sleep+open. The watchdog should measure
                 * time since the last SUCCESSFUL message regardless of
                 * intervening recovery churn. */
                continue;
            }
            /* Other error */
            LOG_ERROR("Read error (errno=%d), continuing...", errno);
            usleep(100000);
            continue;
        }

        last_msg_time = time(NULL);
        process_message(ag, msg);
        free(msg);
    }

    channel_close(ag->channel);
    LOG_INFO("Agent stopped");
    return 0;
}

void agent_stop(agent_t *ag)
{
    if (ag) ag->running = 0;
}

void agent_destroy(agent_t *ag)
{
    if (!ag) return;
    channel_destroy(ag->channel);
    free(ag);
}

/* agent_is_frozen / agent_set_frozen removed — freeze state is managed
 * entirely by cmd-fs.c via static variables. The 'frozen' field in
 * struct agent is unused. */
