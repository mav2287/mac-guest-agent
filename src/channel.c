#include "channel.h"
#include "compat.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <stdio.h>

#define READ_BUF_SIZE 4096
#define POLL_TIMEOUT_MS 1000

struct channel {
    char  *device_path;
    int    fd;       /* read fd */
    int    wfd;      /* write fd (separate to avoid termios conflicts) */
    int    is_open;
    int    is_test;
    int    poll_timeout_ms;
    char   read_buf[READ_BUF_SIZE];
    size_t read_pos;
    size_t read_len;

    /* v2.5.4 diagnostic counters — exposed via SIGUSR1 status dump.
     * Codex (the diagnostic LLM consulted during the issue #10 mitigation
     * design) recommended these so the next reproduction of a "stuck
     * agent" can be split unambiguously between (a) Tiger serial driver
     * not waking select() at all, (b) waking select() but read() returning
     * no useful data, (c) reads succeed but never form a complete
     * \n-delimited message, or (d) writes hanging. Cheap to maintain
     * (~64 bytes per channel) and silent unless dumped. */
    unsigned long select_calls;       /* total select() invocations */
    unsigned long select_timeouts;    /* select() returned 0 (no fd ready) */
    unsigned long select_ready;       /* select() returned > 0 (fd readable) */
    unsigned long read_eagain;        /* post-select read() returned -1/EAGAIN */
    unsigned long read_bytes;         /* sum of bytes successfully read */
    unsigned long messages_extracted; /* complete \n-delimited messages */
    unsigned long reconnects;         /* watchdog or EIO reconnect count */

    /* v2.5.4: read-first probe counters (Codex follow-up recommendation
     * after the first SIGUSR1-dump session caught the wedge cycle).
     * Separate from `read_eagain` so the diagnostic semantics stay clean:
     *
     *   probe_calls   = number of pre-select nonblocking read() attempts
     *   probe_hits    = pre-select reads that returned > 0 bytes
     *                   (= "bytes were waiting in the tty queue that
     *                    select() either had not yet reported, or has
     *                    stopped reporting due to the issue #10 wedge")
     *   probe_eagain  = pre-select reads that returned EAGAIN
     *                   (= no data in the tty queue at that instant)
     *
     * Discriminator on next wedge:
     *
     *   probe_hits rising while select.ready stays flat
     *     -> Tiger's selrecord/readiness path is what's wedged.
     *        The hybrid bypasses it by reading directly. issue #10 fixed
     *        in this case without ever needing the 600s watchdog.
     *
     *   probe_calls climbing but probe_hits == probe_eagain (all empty)
     *     -> data isn't reaching the tty input queue at all (Tiger
     *        driver layer 5-7 or QEMU isa-serial buffering). Read-first
     *        doesn't help; watchdog remains the floor recovery.
     *
     * `last_fionread` exposes ioctl(FIONREAD) at dump time so we can
     * see whether the tty queue has data waiting that neither select()
     * nor a non-blocking read() is exposing — splits the "data is in
     * the queue but invisible" failure from "data hasn't arrived". */
    unsigned long probe_calls;
    unsigned long probe_hits;
    unsigned long probe_eagain;
    int           last_fionread;      /* bytes waiting per FIONREAD at last dump (-1 if unsupported) */
    unsigned long eio_reconnects;     /* EIO-driven reconnects (separate from watchdog `reconnects`) */

    /* v2.5.4 — read() == 0 telemetry (separate from EAGAIN).
     *
     * Empirical finding on Tiger 10.4.11 with the hybrid probe build:
     * the "wedge" PVE-host sees as `qm agent ping = not running` is
     * actually an EIO/reconnect storm triggered by `read()` returning
     * 0 every time PVE's QGA proxy disconnects between calls. For a
     * QEMU isa-serial chardev backed by a unix socket, peer disconnect
     * does NOT mean the device is gone — the chardev's QEMU side stays
     * open, and the next PVE connect-and-write will land in the tty
     * input buffer normally. Treating that 0 as fatal EOF and starting
     * a reconnect ladder is what produced 20+ "Device connection lost"
     * log lines in a row. v2.5.4+ treats `read() == 0` as EAGAIN on
     * the chardev path (see channel_try_read), but we count the events
     * for postmortem visibility. Codex recommendation: keep these as
     * separate counters from `read_eagain` so the dump signal stays
     * legible (the EOF events are the issue #10 root-cause signal,
     * not noise mixed into EAGAIN).
     *
     * Safe even in the presence of a true hardware hangup because our
     * termios sets CLOCAL — XNU's `ttread` only synthesizes EOF for
     * a zombie tty when CLOCAL is off (per bsd/kern/tty.c); with
     * CLOCAL=1 (ignore modem carrier), nonblocking no-data is
     * EWOULDBLOCK, not 0. So a read()==0 here is the chardev case. */
    unsigned long read_eof;           /* post-select read() returned 0 (chardev peer disconnect) */
    unsigned long probe_eof;          /* probe read() returned 0 */

    time_t        last_select_ts;     /* wall clock of last select() return */
    time_t        last_read_ts;       /* wall clock of last successful read() */
    time_t        last_msg_ts;        /* wall clock of last complete message */
};

/* ISA serial is the ONLY supported transport (v2.5.0+).
 *
 * Background: macOS guests run either under Apple's Virtualization.framework
 * (UTM "Virtualize" mode, vz_run) or under plain QEMU/KVM (Proxmox, libvirt,
 * raw QEMU, UTM "Emulate" backend — typically OpenCore-booted). On VZ hosts,
 * Apple's own AppleQEMUGuestAgent is IOKit-launched on the VirtIO console
 * channel via the `AppleVirtIOAgentDevice` match set by `applevirtio.console`
 * — using VirtIO there silently routes traffic to Apple's 18-command agent
 * instead of ours, with no error to the operator. On plain QEMU hosts Apple's
 * driver doesn't load and VirtIO would technically be free, but we don't
 * want a contract where the same agent install behaves differently depending
 * on host class. Result: ISA-only everywhere.
 *
 * v2.4.x had a VirtIO fallback list as a "best effort" path for plain-QEMU
 * configs that didn't present an ISA UART (UTM Emulate defaults, custom QEMU
 * command lines without `-device isa-serial`). v2.5.0 removed that fallback
 * — anyone in that situation now gets a clear "No ISA serial device found"
 * error instead of silently picking VirtIO and (on VZ) connecting to the
 * wrong agent. The fix is documented: add `type=isa` (PVE), an isa-serial
 * device (libvirt), the QemuGuestAgent interface (UTM), or `-device
 * isa-serial` (raw QEMU). Apple16X50Serial.kext has shipped on every macOS
 * from 10.4 onwards with an identical PCI class match, so the requirement
 * is universally satisfiable.
 *
 * Full rationale: docs/COMPATIBILITY.md#isa-serial-transport--why,
 * CHANGELOG v2.5.0 BREAKING section. */
static const char *known_devices[] = {
    "/dev/cu.serial1",
    "/dev/tty.serial1",
    "/dev/cu.serial2",
    "/dev/tty.serial2",
    "/dev/cu.serial",
    "/dev/tty.serial",
    NULL
};

static char *detect_device(void)
{
    struct stat st;
    for (int i = 0; known_devices[i]; i++) {
        if (stat(known_devices[i], &st) == 0 && (st.st_mode & S_IFCHR)) {
            LOG_INFO("Detected serial device: %s", known_devices[i]);
            return strdup(known_devices[i]);
        }
    }
    return NULL;
}

/* Surface the v2.4.x → v2.5.0 transport change explicitly. If a leftover
 * VirtIO device is present (UTM Emulate default, old QEMU config), the
 * agent would have used it in v2.4.x. Now it doesn't — log the device we
 * see, log what to do about it, and bail. The detect_device() loop above
 * only checks ISA paths; this scan is independent and exists purely to
 * produce a useful diagnostic. */
static int log_virtio_diagnostic_if_present(void)
{
    static const char *legacy_virtio_devices[] = {
        "/dev/cu.org.qemu.guest_agent.0",
        "/dev/cu.virtio-console.0",
        "/dev/cu.virtio-serial",
        "/dev/cu.virtio-port",
        "/dev/cu.qemu-guest-agent",
        "/dev/cu.virtio",
        NULL
    };
    struct stat st;
    for (int i = 0; legacy_virtio_devices[i]; i++) {
        if (stat(legacy_virtio_devices[i], &st) == 0 && (st.st_mode & S_IFCHR)) {
            LOG_ERROR("Found VirtIO serial device (%s) but VirtIO transport "
                      "was removed from auto-detect in v2.5.0 — this agent "
                      "now requires ISA serial by default. Reconfigure your "
                      "hypervisor to present an ISA UART: PVE 'qm set "
                      "<vmid> --agent enabled=1,type=isa'; libvirt "
                      "isa-serial device; UTM Serial device with "
                      "Interface=QemuGuestAgent; raw QEMU '-device "
                      "isa-serial'. On Apple Virtualization.framework "
                      "guests (UTM Virtualize mode, vz_run) ISA isn't "
                      "available — switch to the QEMU backend or accept "
                      "Apple's built-in 18-command agent on the VirtIO "
                      "channel instead. If your orchestrator hardcodes "
                      "VirtIO at the libvirt-channel level (e.g., kubevirt) "
                      "and ISA truly isn't an option on this host, see "
                      "docs/NO_ISA_OVERRIDE.md for the gated --virtio "
                      "install path (unsupported configuration, macOS 11+, "
                      "requires SIP disabled).",
                      legacy_virtio_devices[i]);
            return 1;
        }
    }
    return 0;
}

void channel_set_poll_timeout(channel_t *ch, int timeout_ms)
{
    if (ch) ch->poll_timeout_ms = timeout_ms > 0 ? timeout_ms : POLL_TIMEOUT_MS;
}

channel_t *channel_create(const char *device_path)
{
    channel_t *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;
    ch->poll_timeout_ms = POLL_TIMEOUT_MS;
    ch->fd = -1;
    if (device_path) {
        ch->device_path = strdup(device_path);
    }
    return ch;
}

channel_t *channel_create_test(void)
{
    channel_t *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;
    ch->poll_timeout_ms = POLL_TIMEOUT_MS;
    ch->fd = -1;
    ch->is_test = 1;
    return ch;
}

int channel_open(channel_t *ch)
{
    if (!ch) return -1;
    if (ch->is_open) return 0;

    if (ch->is_test) {
        ch->fd = STDIN_FILENO;
        ch->is_open = 1;
        LOG_INFO("Test mode: using stdin/stdout");
        return 0;
    }

    if (!ch->device_path) {
        ch->device_path = detect_device();
        if (!ch->device_path) {
            /* If we found a VirtIO device, log the v2.5.0 transport-change
             * explanation; otherwise fall through to the generic message. */
            if (!log_virtio_diagnostic_if_present()) {
                LOG_ERROR("No ISA serial device found. This agent requires "
                          "ISA serial (v2.5.0+). Checked: /dev/cu.serial1, "
                          "/dev/cu.serial2, /dev/cu.serial. Configure your "
                          "hypervisor accordingly — PVE: 'qm set <vmid> "
                          "--agent enabled=1,type=isa' then fully stop and "
                          "restart the VM; libvirt: add an isa-serial "
                          "device; UTM: add a Serial device with "
                          "Interface=QemuGuestAgent; raw QEMU: pass "
                          "'-device isa-serial'. See README.md > Quick "
                          "Start for the full setup.");
            }
            return -1;
        }
    }

    /* O_NONBLOCK is mandatory for v2.5.4+ (issue #10 hardening per Codex
     * review). Without it, read() blocks indefinitely if select() returned
     * a spurious "ready" signal on Tiger's BSD serial driver — an
     * undocumented but plausible failure mode where the kernel marks the
     * fd readable but the read returns no data. Our read path already
     * handles EAGAIN cleanly (falls back through to the EAGAIN watchdog
     * branch in agent.c), so nonblocking is a strict superset of blocking
     * for this codepath. Also closes the "select lied, read blocked
     * forever" hole that would silently break our watchdog. */
    ch->fd = open(ch->device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (ch->fd < 0) {
        LOG_ERROR("Failed to open device %s: %s", ch->device_path, strerror(errno));
        return -1;
    }

    /* Re-assert non-blocking after open in case tcsetattr() or any other
     * subsequent ioctl resets it (some BSD serial drivers do). */
    int flags = fcntl(ch->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(ch->fd, F_SETFL, flags | O_NONBLOCK);

    compat_cloexec(ch->fd);

    /* For serial ports: full raw mode on a single fd.
     * Disable ALL input and output processing:
     * - ICANON off: no canonical (line) mode
     * - ECHO off: no echo
     * - ISTRIP off: preserve 8th bit (0xFF)
     * - OPOST off: no \n → \r\n conversion
     * - IXON/IXOFF off: no software flow control
     * This matches Linux qemu-ga ISA serial configuration. */
    ch->wfd = ch->fd;
    if (isatty(ch->fd)) {
        struct termios tio;
        if (tcgetattr(ch->fd, &tio) == 0) {
            tio.c_iflag = 0;                       /* No input processing */
            tio.c_oflag = 0;                       /* No output processing */
            tio.c_lflag = 0;                       /* No line discipline */
            tio.c_cflag = CS8 | CREAD | CLOCAL;   /* 8-bit, rx on, ignore modem */
            tio.c_cc[VMIN] = 1;
            tio.c_cc[VTIME] = 0;
            /* Set max baud rate. QEMU ignores baud rate on virtual serial ports
             * (data flows at memory speed), but the macOS Apple16X50Serial.kext
             * may use it to pace internal buffering. 115200 is the standard
             * max for 16550 UARTs and is widely supported across all macOS versions. */
            cfsetispeed(&tio, B115200);
            cfsetospeed(&tio, B115200);
            tcsetattr(ch->fd, TCSANOW, &tio);
            /* v2.5.4 (Codex post-EOF-storm fix): flush OUTPUT only, not
             * input. Input flush on reopen can drop PVE's incoming
             * `guest-sync-delimited` recovery command if it races our
             * reopen window — QGA protocol defines guest-sync-delimited
             * as the host's stream-recovery handshake (sent after host
             * reconnects), and we don't want to flush it before
             * processing it. Output flush is fine: stale response bytes
             * from a prior PVE session we never finished should be
             * dropped. */
            tcflush(ch->fd, TCOFLUSH);
            LOG_INFO("Serial port: full raw mode (single fd=%d)", ch->fd);
        }
    }

    ch->is_open = 1;
    ch->read_pos = 0;
    ch->read_len = 0;
    LOG_INFO("Opened device: %s (fd=%d)", ch->device_path, ch->fd);
    return 0;
}


void channel_flush_stale_output(channel_t *ch)
{
    if (!ch || !ch->is_open || ch->is_test || ch->fd < 0) return;
    /* Discard any pending output bytes in the serial transmit buffer.
     * This clears stale responses from previous PVE sessions that
     * disconnected before reading all data. Only touches OUTPUT —
     * the input buffer (where the next command may be waiting) is untouched. */
    tcflush(ch->fd, TCOFLUSH);
}

void channel_close(channel_t *ch)
{
    if (!ch || !ch->is_open) return;

    if (!ch->is_test && ch->fd >= 0) {
        close(ch->fd);
    }
    ch->fd = -1;
    ch->wfd = -1;
    ch->is_open = 0;
    ch->read_pos = 0;
    ch->read_len = 0;
    LOG_INFO("Channel closed");
}

void channel_destroy(channel_t *ch)
{
    if (!ch) return;
    channel_close(ch);
    free(ch->device_path);
    free(ch);
}

int channel_is_open(channel_t *ch)
{
    return ch && ch->is_open;
}

/* v2.5.4 — pre-select read-first probe helper (Codex follow-up).
 *
 * Attempts a single non-blocking read() into the channel's buffer,
 * shifting/clearing the buffer first if needed. Used in two places:
 *
 *   probe=1: invoked BEFORE select() to bypass Tiger's `selrecord`
 *            wakeup mechanism when it has gone deaf (issue #10).
 *            Counted in probe_calls/probe_hits/probe_eagain.
 *
 *   probe=0: invoked AFTER select() returned readable. Counted in
 *            the existing read_eagain/read_bytes counters so the
 *            diagnostic dump's pre-v2.5.4 fields keep their meaning.
 *
 * Return values:
 *    1 = bytes were read into the buffer; caller should extract a line
 *    0 = EAGAIN/EWOULDBLOCK; nothing read; caller decides what's next
 *   -1 = EOF or genuine read error; channel is dead, errno = EIO,
 *        caller should return NULL to trigger the reconnect path
 */
static int channel_try_read(channel_t *ch, int probe)
{
    if (probe)
        ch->probe_calls++;

    /* Buffer management: keep the buffer healthy whether the bytes are
     * coming from the probe or from the post-select path. */
    if (ch->read_len >= READ_BUF_SIZE - 1) {
        LOG_WARN("Read buffer overflow, discarding data");
        ch->read_pos = 0;
        ch->read_len = 0;
    }
    if (ch->read_pos > 0 && ch->read_len > 0) {
        memmove(ch->read_buf, ch->read_buf + ch->read_pos, ch->read_len);
        ch->read_pos = 0;
    }

    ssize_t n = read(ch->fd, ch->read_buf + ch->read_len,
                     READ_BUF_SIZE - ch->read_len - 1);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            if (probe) ch->probe_eagain++;
            else       ch->read_eagain++;
            errno = EAGAIN;
            return 0;
        }
        LOG_ERROR("read() error: %s", strerror(errno));
        errno = EIO;
        return -1;
    }

    if (n == 0) {
        /* v2.5.4 — DO NOT treat read() == 0 as EOF/dead-device.
         *
         * For a QEMU isa-serial chardev backed by a unix socket, this
         * happens every time the host-side peer (PVE QGA proxy)
         * disconnects between calls. The chardev itself is still open
         * at the QEMU layer — Tiger's serial driver just has nothing
         * in the input queue right this instant. The next host-side
         * connect-and-write will land normally and select() will wake.
         *
         * Treating this as EOF was the actual issue #10 wedge mechanism:
         * read() == 0 -> errno=EIO -> agent.c EIO branch -> close + sleep 5
         * + reopen -> immediately read() == 0 again -> repeat forever.
         * 20+ "Device connection lost" log lines per minute, the 600s
         * watchdog never fires because we reset its timer each EIO cycle.
         *
         * Empirical correction: treat as EAGAIN. Count it in a separate
         * counter (read_eof/probe_eof) so the dump signal stays legible.
         * Don't log per occurrence — it's normal-cadence noise on the
         * chardev disconnect/reconnect cycle PVE QGA does between calls.
         * The watchdog still covers a genuinely-dead device — if no
         * complete message arrives for 600s, it'll close+reopen anyway. */
        if (probe) ch->probe_eof++;
        else       ch->read_eof++;
        errno = EAGAIN;
        return 0;
    }

    if (probe)
        ch->probe_hits++;

    ch->read_bytes += (unsigned long)n;
    ch->last_read_ts = time(NULL);
    ch->read_len += (size_t)n;
    ch->read_buf[ch->read_len] = '\0';
    return 1;
}

/* v2.5.4 diagnostic: dump per-channel counters via LOG_INFO.
 *
 * Designed to be safe to call from a signal handler (uses LOG_INFO which
 * goes through our logging glue — see signal handler in main.c for the
 * async-signal-safe call pattern: the handler sets a flag, the main loop
 * calls this function). Output format is single-line, grep-friendly:
 *
 *   [INFO] channel_status fd=5 open=1 select=1200/1198/2 read=84/12 \
 *          probe=300/14/286 msgs=2 reconnects=0 buf_len=0 fionread=0 \
 *          ages=37/41/87
 *
 * Field semantics:
 *   select=<calls>/<timeouts>/<ready>          select() syscall behavior
 *   read=<bytes>/<eagain_count>                post-select read() outcomes
 *   probe=<calls>/<hits>/<eagain>              pre-select read-first probe (v2.5.4+)
 *   msgs=<complete_lines_extracted>            \n-delimited lines pulled
 *   reconnects=<watchdog_or_EIO_count>         channel close+open cycles
 *   buf_len=<currently_buffered_bytes>         bytes awaiting newline
 *   fionread=<bytes_in_tty_input_queue>        ioctl(FIONREAD) at dump time, -1 if unsupported
 *   ages=<sec_since_last_select>/<since_last_read>/<since_last_msg>
 *
 * On a healthy idle agent you'd expect select.calls and select.timeouts
 * to climb together (one per second) with read.bytes and msgs flat unless
 * a peer is sending commands. probe.calls climbs with the loop too; on
 * healthy systems probe.hits should be a small fraction of probe.calls
 * because select() usually still fires correctly.
 *
 * On a wedged Tiger driver (issue #10), the signatures distinguish:
 *
 *   select.ready stuck flat AND probe.hits rising
 *     -> Tiger's selrecord readiness wakeup is what's wedged.
 *        The read-first probe is bypassing it. issue #10 fixed at this
 *        layer without the watchdog needing to fire.
 *
 *   select.ready stuck flat AND probe.hits flat AND probe.eagain rising
 *     -> No bytes in the tty input queue. Wedge is at a deeper layer
 *        (Tiger Apple16X50Serial.kext interrupt/buffer or QEMU isa-serial
 *        emulation). Read-first can't help; watchdog is the floor.
 *
 *   FIONREAD > 0 but probe still gets EAGAIN
 *     -> Tiger's tty queue has data but read() refuses to deliver it.
 *        Bug below the BSD read syscall itself. Very rare.
 *
 *   select.calls itself frozen across two dumps in time
 *     -> The agent loop is stuck in syscall, not in our code. Need a
 *        kernel-level watchdog (KeepAlive, external process monitor).
 */
void channel_dump_status(channel_t *ch)
{
    if (!ch) {
        LOG_INFO("channel_status: ch=NULL");
        return;
    }

    /* Snapshot FIONREAD at dump time: how many bytes are in the tty's
     * input queue right now. Critical splitter for the wedge mechanism:
     * if FIONREAD > 0 but our probe is returning EAGAIN, the bug is in
     * Tiger's tty -> read() delivery, NOT in selrecord. */
    if (ch->is_open && ch->fd >= 0 && !ch->is_test) {
        int avail = -1;
        if (ioctl(ch->fd, FIONREAD, &avail) == 0)
            ch->last_fionread = avail;
        else
            ch->last_fionread = -1;
    }

    time_t now = time(NULL);
    long age_sel = ch->last_select_ts ? (long)(now - ch->last_select_ts) : -1;
    long age_rd  = ch->last_read_ts   ? (long)(now - ch->last_read_ts)   : -1;
    long age_msg = ch->last_msg_ts    ? (long)(now - ch->last_msg_ts)    : -1;
    LOG_INFO("channel_status fd=%d open=%d test=%d select=%lu/%lu/%lu "
             "read=%lu/%lu/%lu probe=%lu/%lu/%lu/%lu msgs=%lu "
             "reconnects=wd:%lu/eio:%lu buf_len=%zu fionread=%d "
             "ages=%ld/%ld/%ld",
             ch->fd, ch->is_open, ch->is_test,
             ch->select_calls, ch->select_timeouts, ch->select_ready,
             ch->read_bytes, ch->read_eagain, ch->read_eof,
             ch->probe_calls, ch->probe_hits, ch->probe_eagain, ch->probe_eof,
             ch->messages_extracted,
             ch->reconnects, ch->eio_reconnects,
             ch->read_len, ch->last_fionread,
             age_sel, age_rd, age_msg);
}

void channel_note_eio_reconnect(channel_t *ch)
{
    if (ch) ch->eio_reconnects++;
}

void channel_note_reconnect(channel_t *ch)
{
    if (ch) ch->reconnects++;
}

const char *channel_get_path(channel_t *ch)
{
    return ch ? ch->device_path : NULL;
}

/* Read a line from the channel. Returns malloc'd string or NULL. */
char *channel_read_message(channel_t *ch)
{
    /* v2.5.4: set errno explicitly when channel isn't open (Codex review).
     * Previously the caller saw NULL with whatever errno happened to be
     * set, which could be 0 — and agent.c's outer loop only treats
     * `errno == EIO` as "trigger reconnect". Without EIO, the loop falls
     * into the "Other error" branch and just usleeps + retries against
     * the same closed channel forever. Returning EIO routes us correctly
     * into the reconnect/sleep-5 path. */
    if (!ch) { errno = EIO; return NULL; }
    if (!ch->is_open) { errno = EIO; return NULL; }

    if (ch->is_test) {
        /* Test mode: blocking read from stdin */
        fprintf(stdout, "QMP> ");
        fflush(stdout);

        char line[READ_BUF_SIZE];
        if (!fgets(line, sizeof(line), stdin)) {
            if (feof(stdin)) {
                errno = 0;
                return NULL;
            }
            errno = EIO;
            return NULL;
        }
        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            errno = EAGAIN;
            return NULL;
        }
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            errno = 0;
            return NULL;
        }
        return strdup(line);
    }

    /* Check if we already have a complete line in the buffer BEFORE waiting.
     * PVE sends sync-delimited + ping in ONE write. If we read both into
     * our buffer but only extracted the first line, the second line is
     * already here — no need to wait on select(). */
    if (ch->read_len > 0 && memchr(ch->read_buf, '\n', ch->read_len)) {
        goto extract_line;
    }

    /* v2.5.4 — read-first probe BEFORE select().
     *
     * The agent loop's previous select+read pattern depended on Tiger's
     * `selrecord` mechanism firing a wakeup on data arrival. Issue #10
     * is exactly the failure mode where that wakeup mechanism goes
     * deaf for hours at a time: select() keeps returning 0 (timeout)
     * even though the host (PVE QGA proxy) is still writing bytes to
     * our serial fd. The watchdog below catches it after 600s, but it
     * IS a band-aid — it doesn't fix the read path, just resets it.
     *
     * The read-first probe is a "different route" suggested by Codex
     * after looking at the SIGUSR1-dump evidence of the wedge cycle:
     * before the readiness syscall, do one nonblocking read(). If the
     * bytes are in the tty input queue (the selrecord layer just
     * stopped reporting them), the read returns them immediately and
     * we skip select() entirely — wedge becomes a non-event.
     *
     * If the read returns EAGAIN, we fall through to the existing
     * select() path. On healthy systems this adds one extra syscall
     * per loop iteration (microseconds — negligible). The probe is
     * counted separately from the post-select read_eagain so the
     * diagnostic dumps still tell us what stage is stalled. */
    int pr = channel_try_read(ch, 1 /*probe*/);
    if (pr < 0) return NULL;          /* EOF/EIO bubbled up */
    if (pr > 0) goto extract_line;    /* probe got bytes -- skip select() */

    /* Device mode: select + read.
     *
     * select(), not poll(): macOS poll() is implemented on top of kqueue,
     * and the serial BSD client on Mac OS X 10.4 Tiger does not support the
     * kqueue readiness path — poll() returns POLLNVAL (0x20) for a valid,
     * open serial fd (confirmed on 10.4.11, issue #2). The agent treated
     * that as a fatal device error and reconnect-looped forever without
     * ever reading a byte. select() uses the legacy selrecord path the
     * driver does implement and works on every macOS version. A single fd
     * is well within FD_SETSIZE.
     *
     * In v2.5.4+ this select() is the SECOND chance — the read-first
     * probe above already tried to consume bytes directly. We end up
     * here only if the tty queue was empty at probe time. We still
     * select() to block efficiently up to poll_timeout_ms rather than
     * burn CPU in a tight nonblocking loop. */
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ch->fd, &readfds);

    struct timeval tv;
    tv.tv_sec  = ch->poll_timeout_ms / 1000;
    tv.tv_usec = (ch->poll_timeout_ms % 1000) * 1000;

    ch->select_calls++;
    int ret = select(ch->fd + 1, &readfds, NULL, NULL, &tv);
    ch->last_select_ts = time(NULL);
    if (ret < 0) {
        if (errno == EINTR) {
            errno = EAGAIN;
            return NULL;
        }
        /* A genuine select() failure (e.g. EBADF) means the fd is gone —
         * fail to the reconnect path rather than spinning on a dead fd. */
        LOG_ERROR("select() error: %s", strerror(errno));
        errno = EIO;
        return NULL;
    }
    if (ret == 0) {
        /* Timeout - normal, no data */
        ch->select_timeouts++;
        errno = EAGAIN;
        return NULL;
    }
    ch->select_ready++;

    /* select() said ready — read the actual bytes. Same helper as the
     * probe, just counted separately (probe=0). */
    int sr = channel_try_read(ch, 0 /*post-select*/);
    if (sr < 0) return NULL;
    if (sr == 0) {
        /* select() said ready but read() returned EAGAIN. Shouldn't
         * happen on a sane driver — but on Tiger it has been seen.
         * Treat as a normal "nothing usable yet" tick. */
        errno = EAGAIN;
        return NULL;
    }

extract_line:
    ;  /* Look for a complete line */
    char *newline = memchr(ch->read_buf, '\n', ch->read_len);
    if (!newline) {
        errno = EAGAIN;
        return NULL;
    }
    ch->messages_extracted++;
    ch->last_msg_ts = time(NULL);

    size_t line_len = (size_t)(newline - ch->read_buf);
    char *line = malloc(line_len + 1);
    if (!line) return NULL;

    memcpy(line, ch->read_buf, line_len);
    line[line_len] = '\0';

    LOG_DEBUG("Received %zu bytes", line_len);

    /* Advance past the newline */
    size_t consumed = line_len + 1;
    ch->read_pos = 0;
    ch->read_len -= consumed;
    if (ch->read_len > 0) {
        memmove(ch->read_buf, ch->read_buf + consumed, ch->read_len);
    }

    /* Trim CR if present */
    if (line_len > 0 && line[line_len - 1] == '\r')
        line[--line_len] = '\0';

    /* Skip empty lines and 0xFF delimiters */
    char *p = line;
    while (*p == '\xff') p++;
    if (*p == '\0') {
        free(line);
        errno = EAGAIN;
        return NULL;
    }

    if (p != line) {
        char *trimmed = strdup(p);
        free(line);
        return trimmed;
    }

    return line;
}

static int channel_write_all(channel_t *ch, const void *data, size_t len)
{
    int fd = ch->is_test ? STDOUT_FILENO : ch->wfd;
    const char *p = data;
    /* write loop */
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* select(), not poll() — see channel_read_message(). */
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(fd, &writefds);
                struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
                select(fd + 1, NULL, &writefds, NULL, &tv);
                continue;
            }
            LOG_ERROR("Serial write failed: %s", strerror(errno));
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int channel_send_response(channel_t *ch, const char *data)
{
    if (!ch || !ch->is_open || !data) return -1;

    size_t len = strlen(data);
    if (channel_write_all(ch, data, len) < 0) return -1;
    if (channel_write_all(ch, "\n", 1) < 0) return -1;

    if (!ch->is_test && ch->wfd >= 0)
        tcdrain(ch->wfd);

    /* Hex dump of what we sent */
    LOG_DEBUG("Sent response (%zu bytes): %s", len, data);
    return 0;
}

int channel_send_delimited_response(channel_t *ch, const char *data)
{
    if (!ch || !ch->is_open || !data) return -1;

    unsigned char delim = 0xFF;
    if (channel_write_all(ch, &delim, 1) < 0) return -1;

    size_t len = strlen(data);
    if (channel_write_all(ch, data, len) < 0) return -1;
    if (channel_write_all(ch, "\n", 1) < 0) return -1;

    if (!ch->is_test && ch->wfd >= 0)
        tcdrain(ch->wfd);

    LOG_DEBUG("Sent delimited response (%zu bytes)", len);
    return 0;
}
