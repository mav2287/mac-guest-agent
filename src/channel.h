#ifndef MGA_CHANNEL_H
#define MGA_CHANNEL_H

#include <sys/types.h>

typedef struct channel channel_t;

/* Create a channel for a device path (NULL = auto-detect) */
channel_t *channel_create(const char *device_path);

/* Create a test channel using stdin/stdout */
channel_t *channel_create_test(void);

/* Open the channel. Returns 0 on success. */
int channel_open(channel_t *ch);

/* Close the channel */
void channel_close(channel_t *ch);

/* Free the channel */
void channel_destroy(channel_t *ch);

/* Read a single line (JSON message). Returns malloc'd string or NULL.
   Returns NULL with errno=EAGAIN on timeout (normal). */
char *channel_read_message(channel_t *ch);

/* Send a response string (appends newline) */
int channel_send_response(channel_t *ch, const char *data);

/* Send a delimited response (0xFF prefix + data + newline) */
int channel_send_delimited_response(channel_t *ch, const char *data);


/* Flush stale output from previous sessions (TCOFLUSH only, does not touch input) */
void channel_flush_stale_output(channel_t *ch);

/* Check if channel is open */
int channel_is_open(channel_t *ch);

/* Get device path */
const char *channel_get_path(channel_t *ch);

#endif
