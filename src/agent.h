#ifndef MGA_AGENT_H
#define MGA_AGENT_H

#include "channel.h"

#define AGENT_VERSION "2.3.0"

typedef struct agent agent_t;

/* Create agent. device_path=NULL for auto-detect, test_mode for stdin/stdout */
agent_t *agent_create(const char *device_path, int test_mode);

/* Run the agent main loop (blocks until stopped) */
int agent_run(agent_t *ag);

/* Signal the agent to stop */
void agent_stop(agent_t *ag);

/* Destroy the agent */
void agent_destroy(agent_t *ag);

/* Get/set frozen state (for fsfreeze simulation) */
int agent_is_frozen(agent_t *ag);
void agent_set_frozen(agent_t *ag, int frozen);

#endif
