#ifndef MGA_COMMANDS_H
#define MGA_COMMANDS_H

#include "third_party/cJSON.h"

/* Command handler function signature.
   Takes arguments (may be NULL), returns cJSON* result (caller frees) or NULL.
   On error, set *err_class and *err_desc to static strings and return NULL. */
typedef cJSON *(*command_handler_fn)(cJSON *args, const char **err_class, const char **err_desc);

typedef struct {
    const char       *name;
    command_handler_fn handler;
    int               enabled;
} command_entry_t;

/* Initialize all commands (called once at startup) */
void commands_init(void);

/* Apply block-rpcs and allow-rpcs filters.
   block_rpcs: comma-separated list of commands to disable.
   allow_rpcs: comma-separated list of commands to allow (all others disabled).
   If both are set, allow_rpcs takes precedence. */
void commands_apply_filters(const char *block_rpcs, const char *allow_rpcs);

/* Dispatch a parsed QMP request. Returns a JSON response string (caller frees). */
char *commands_dispatch(const char *cmd_name, cJSON *args, const cJSON *id);

/* Get the full list of registered commands for guest-info.
   Returns a cJSON array. Caller frees with cJSON_Delete. */
cJSON *commands_get_list(void);

/* Get the number of registered commands */
int commands_count(void);

/* Register a single command. Called by cmd-*.c init functions. */
void command_register(const char *name, command_handler_fn handler, int enabled);

#endif
