#ifndef MGA_SERVICE_H
#define MGA_SERVICE_H

#define SERVICE_NAME    "com.macos.guest-agent"
#define BINARY_PATH     "/usr/local/bin/mac-guest-agent"
#define PLIST_PATH      "/Library/LaunchDaemons/com.macos.guest-agent.plist"
#define LOG_PATH        "/var/log/mac-guest-agent.log"
#define SHARE_PATH      "/usr/local/share/mac-guest-agent"

/* All three handlers accept a `dry_run` flag (added in v2.5.1). When set,
 * the function performs every non-destructive check (path validation,
 * permission inspection, etc.) and prints the actions it WOULD take —
 * but does not touch the filesystem or call launchctl. Root is also
 * skipped in dry-run mode because no privileged operations execute.
 * Used by scripts/install.sh --dry-run for end-to-end smoke testing
 * the install flow without affecting the host. */
int service_install(int dry_run);
int service_uninstall(int dry_run);

/* Update from a local binary file. Stops service, replaces binary, restarts.
 * dry_run flag: see comment above. */
int service_update(const char *new_binary_path, int dry_run);

#endif
