#include "channel.h"
#include "compat.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
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
};

static const char *known_devices[] = {
    /* ISA serial — primary transport. Required on all macOS versions because
     * Big Sur+ ships Apple's own VirtIO guest agent which claims the default
     * VirtIO serial channel. ISA serial via Apple16X50Serial.kext is the only
     * channel Apple's agent doesn't claim.
     * Covers: PVE (agent type=isa), plain QEMU (-device isa-serial), libvirt */
    "/dev/cu.serial1",
    "/dev/tty.serial1",
    "/dev/cu.serial2",
    "/dev/tty.serial2",
    "/dev/cu.serial",
    "/dev/tty.serial",
    /* VirtIO serial — only works if Apple's built-in agent is not present
     * (pre-Big Sur) or if specifically configured to avoid conflict.
     * Kept for edge cases and UTM compatibility. */
    "/dev/cu.org.qemu.guest_agent.0",
    "/dev/tty.org.qemu.guest_agent.0",
    "/dev/cu.virtio-console.0",
    "/dev/tty.virtio-console.0",
    "/dev/cu.virtio-serial",
    "/dev/tty.virtio-serial",
    "/dev/cu.virtio-port",
    "/dev/tty.virtio-port",
    "/dev/cu.qemu-guest-agent",
    "/dev/tty.qemu-guest-agent",
    /* UTM (Apple Virtualization.framework) */
    "/dev/cu.virtio",
    "/dev/tty.virtio",
    NULL
};

static char *detect_device(void)
{
    struct stat st;
    for (int i = 0; known_devices[i]; i++) {
        if (stat(known_devices[i], &st) == 0 && (st.st_mode & S_IFCHR)) {
            const char *transport = "serial";
            if (strstr(known_devices[i], "virtio") || strstr(known_devices[i], "org.qemu"))
                transport = "virtio";
            LOG_INFO("Detected %s device: %s", transport, known_devices[i]);
            return strdup(known_devices[i]);
        }
    }
    return NULL;
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
            LOG_ERROR("No serial device found. Ensure PVE has 'agent: enabled=1,type=isa' and VM was fully stopped and restarted.");
            return -1;
        }
    }

    ch->fd = open(ch->device_path, O_RDWR | O_NOCTTY);
    if (ch->fd < 0) {
        LOG_ERROR("Failed to open device %s: %s", ch->device_path, strerror(errno));
        return -1;
    }

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
            tcflush(ch->fd, TCIOFLUSH);
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

const char *channel_get_path(channel_t *ch)
{
    return ch ? ch->device_path : NULL;
}

/* Read a line from the channel. Returns malloc'd string or NULL. */
char *channel_read_message(channel_t *ch)
{
    if (!ch || !ch->is_open) return NULL;

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

    /* Check if we already have a complete line in the buffer BEFORE polling.
     * PVE sends sync-delimited + ping in ONE write. If we read both into
     * our buffer but only extracted the first line, the second line is
     * already here — no need to wait for poll(). */
    if (ch->read_len > 0 && memchr(ch->read_buf, '\n', ch->read_len)) {
        goto extract_line;
    }

    /* Device mode: poll + read */
    struct pollfd pfd;
    pfd.fd = ch->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, ch->poll_timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) {
            errno = EAGAIN;
            return NULL;
        }
        LOG_ERROR("poll() error: %s", strerror(errno));
        return NULL;
    }
    if (ret == 0) {
        /* Timeout - normal, no data */
        errno = EAGAIN;
        return NULL;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        LOG_ERROR("Device error/hangup (revents=0x%x)", pfd.revents);
        errno = EIO;
        return NULL;
    }

    /* Read available data into buffer */
    if (ch->read_len >= READ_BUF_SIZE - 1) {
        /* Buffer full without a newline — discard */
        LOG_WARN("Read buffer overflow, discarding data");
        ch->read_pos = 0;
        ch->read_len = 0;
    }

    /* Shift remaining data to front if needed */
    if (ch->read_pos > 0 && ch->read_len > 0) {
        memmove(ch->read_buf, ch->read_buf + ch->read_pos, ch->read_len);
        ch->read_pos = 0;
    }

    ssize_t n = read(ch->fd, ch->read_buf + ch->read_len, READ_BUF_SIZE - ch->read_len - 1);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            errno = EAGAIN;
            return NULL;
        }
        LOG_ERROR("read() error: %s", strerror(errno));
        return NULL;
    }
    if (n == 0) {
        LOG_ERROR("Device returned EOF");
        errno = EIO;
        return NULL;
    }

    ch->read_len += (size_t)n;
    ch->read_buf[ch->read_len] = '\0';

extract_line:
    ;  /* Look for a complete line */
    char *newline = memchr(ch->read_buf, '\n', ch->read_len);
    if (!newline) {
        errno = EAGAIN;
        return NULL;
    }

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
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                poll(&pfd, 1, 5000);
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
