#ifndef MGA_AGENT_H
#define MGA_AGENT_H

#include "channel.h"
#include <signal.h>

/* AGENT_VERSION is provided by the Makefile via -DVERSION="x.y.z".
 * Fallback if compiling without the Makefile: */
#ifndef VERSION
#define VERSION "dev"
#endif
#define AGENT_VERSION VERSION

typedef struct agent agent_t;

/* Create agent. device_path=NULL for auto-detect, test_mode for stdin/stdout */
agent_t *agent_create(const char *device_path, int test_mode);

/* Run the agent main loop (blocks until stopped).
 * stop_flag: optional pointer to a volatile sig_atomic_t that signals shutdown.
 * Pass NULL if not using signal-based shutdown. */
int agent_run(agent_t *ag, volatile sig_atomic_t *stop_flag);

/* Signal the agent to stop */
void agent_stop(agent_t *ag);

/* Destroy the agent */
void agent_destroy(agent_t *ag);

/* Freeze state managed by cmd-fs.c, not here */

#endif
