#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <dlfcn.h>

/*
 * Logging with os_log support on macOS 10.12+.
 *
 * We weak-link to os_log at runtime so the binary still runs on older macOS.
 * If os_log is available, messages go to both os_log (visible in Console.app
 * and `log show`) AND the file/stderr. If not available, file/stderr only.
 */

/* os_log function types (from <os/log.h>, but we load dynamically) */
typedef struct os_log_s *os_log_t;
typedef enum {
    OS_LOG_TYPE_DEFAULT = 0,
    OS_LOG_TYPE_INFO    = 1,
    OS_LOG_TYPE_DEBUG   = 2,
    OS_LOG_TYPE_ERROR   = 16,
    OS_LOG_TYPE_FAULT   = 17
} os_log_type_t;

typedef os_log_t (*os_log_create_fn)(const char *subsystem, const char *category);
typedef void (*os_log_impl_fn)(void *dso, os_log_t log, os_log_type_t type,
                                const char *format, uint8_t *buf, uint32_t size);

static FILE *log_fp = NULL;
static log_level_t current_level = LOG_INFO;
static int os_log_available = 0;
static os_log_t os_log_handle = NULL;
static os_log_create_fn fn_os_log_create = NULL;

/* We use os_log_with_type via a simple wrapper since the variadic
   _os_log_impl is complex. Instead, use os_log_error/os_log_info
   equivalents by calling os_log_with_type. */
typedef void (*os_log_with_type_fn)(os_log_t log, os_log_type_t type, const char *msg);

static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static void init_os_log(void)
{
    /* Try to load os_log dynamically */
    fn_os_log_create = (os_log_create_fn)dlsym(RTLD_DEFAULT, "os_log_create");
    if (fn_os_log_create) {
        os_log_handle = fn_os_log_create("com.github.mac-guest-agent", "agent");
        if (os_log_handle) {
            os_log_available = 1;
        }
    }
}

/* Send a message to os_log using the C _os_log_impl function.
   Since the os_log API uses complex __builtin_os_log_format,
   we use a simpler approach: NSLog-style via syslog. */
static void send_to_os_log(log_level_t level, const char *message)
{
    if (!os_log_available) return;

    /* Use syslog() as the os_log bridge — messages appear in Console.app
       and `log show` on macOS 10.12+. This avoids the complex
       _os_log_impl ABI while still integrating with unified logging. */
    int syslog_level;
    switch (level) {
    case LOG_DEBUG: syslog_level = 7; break;  /* LOG_DEBUG */
    case LOG_INFO:  syslog_level = 6; break;  /* LOG_INFO */
    case LOG_WARN:  syslog_level = 4; break;  /* LOG_WARNING */
    case LOG_ERROR: syslog_level = 3; break;  /* LOG_ERR */
    case LOG_FATAL: syslog_level = 2; break;  /* LOG_CRIT */
    default:        syslog_level = 6; break;
    }
    /* syslog is available on all macOS versions */
    extern void syslog(int, const char *, ...);
    syslog(syslog_level, "%s", message);
}

void log_init(const char *log_file, log_level_t level)
{
    current_level = level;

    /* Initialize os_log / syslog integration */
    init_os_log();

    if (log_file) {
        log_fp = fopen(log_file, "a");
        if (!log_fp) {
            fprintf(stderr, "Failed to open log file: %s\n", log_file);
        }
    }
}

void log_close(void)
{
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void log_set_level(log_level_t level)
{
    current_level = level;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level < current_level)
        return;

    /* Format the message */
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    /* Send to os_log / syslog (visible in Console.app on 10.12+) */
    if (os_log_available) {
        send_to_os_log(level, message);
    }

    /* Also write to file/stderr for direct access */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    FILE *out = log_fp ? log_fp : stderr;

    fprintf(out, "%s [%s] %s\n", time_str, level_names[level], message);
    fflush(out);

    /* LOG_FATAL: caller is responsible for cleanup and exit.
     * Do not call exit() here — it skips cleanup (pidfile, freeze state, etc). */
}
