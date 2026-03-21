#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static FILE *log_fp = NULL;
static log_level_t current_level = LOG_INFO;

static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

void log_init(const char *log_file, log_level_t level)
{
    current_level = level;
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

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    FILE *out = log_fp ? log_fp : stderr;

    fprintf(out, "%s [%s] ", time_str, level_names[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

    if (level == LOG_FATAL) {
        exit(1);
    }
}
