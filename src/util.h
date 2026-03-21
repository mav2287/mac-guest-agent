#ifndef MGA_UTIL_H
#define MGA_UTIL_H

#include <sys/types.h>

/* Run a shell command and capture stdout. Caller frees *output. Returns exit code. */
int run_command_capture(const char *cmd, char **output);

/* Run a shell command, discard output. Returns exit code. */
int run_command(const char *cmd);

/* Run a command with explicit argv. Capture stdout+stderr if capture != NULL.
   Caller frees *out_buf. Returns exit code, or -1 on fork/exec failure. */
int run_command_v(const char *path, char *const argv[], char **out_buf, size_t *out_len);

/* Base64 encode. Returns malloc'd string. */
char *base64_encode(const unsigned char *data, size_t len);

/* Base64 decode. Returns malloc'd buffer, sets *out_len. */
unsigned char *base64_decode(const char *input, size_t *out_len);

/* Read entire file into malloc'd buffer. Caller frees. Returns NULL on error. */
char *read_file(const char *path, size_t *out_len);

/* Write data to file, creating/truncating. Returns 0 on success. */
int write_file(const char *path, const char *data, size_t len, int mode);

/* Safe string duplicate */
char *safe_strdup(const char *s);

/* Trim leading and trailing whitespace in-place */
char *str_trim(char *s);

#endif
