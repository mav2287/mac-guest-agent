#ifndef MGA_CMD_FS_H
#define MGA_CMD_FS_H

/* Set test/dry-run mode — freeze simulates without touching real filesystems */
void fsfreeze_set_test_mode(int enabled);

/* Called from agent.c poll loop to check if frozen and run continuous sync */
int fsfreeze_is_frozen(void);
void fsfreeze_continuous_sync(void);

/* Called from agent.c to check if a command is allowed during freeze */
int fsfreeze_command_allowed(const char *cmd_name);

#endif
