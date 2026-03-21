#ifndef MGA_LOG_H
#define MGA_LOG_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

void log_init(const char *log_file, log_level_t level);
void log_close(void);
void log_set_level(log_level_t level);

void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(fmt, ...) log_msg(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_msg(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) log_msg(LOG_FATAL, fmt, ##__VA_ARGS__)

#endif
