#include "service.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Embedded plist data - generated at build time by xxd */
#include "plist_data.h"

/* Helpers that gate each side-effect on the dry_run flag. Used by the
 * three service_* handlers below. When dry_run is set, the helper prints
 * "DRY RUN: would ..." and returns success without performing the action.
 * v2.5.1 — see scripts/install.sh --dry-run for the script-side counterpart. */
static int dr_mkdir_p(int dry_run, const char *path, mode_t mode);
static int dr_run_command(int dry_run, const char *cmd);
static int dr_write_file(int dry_run, const char *path, const char *data,
                         size_t len, mode_t mode);

static int mkdir_p(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 0;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

static int dr_mkdir_p(int dry_run, const char *path, mode_t mode)
{
    if (dry_run) {
        printf("DRY RUN: would mkdir -p %s (mode 0%o)\n", path, mode);
        return 0;
    }
    return mkdir_p(path, mode);
}

static int dr_run_command(int dry_run, const char *cmd)
{
    if (dry_run) {
        printf("DRY RUN: would run: %s\n", cmd);
        return 0;
    }
    return run_command(cmd);
}

static int dr_write_file(int dry_run, const char *path, const char *data,
                         size_t len, mode_t mode)
{
    if (dry_run) {
        printf("DRY RUN: would write %zu bytes to %s (mode 0%o)\n",
               len, path, mode);
        return 0;
    }
    return write_file(path, data, len, mode);
}

static void stop_existing(int dry_run)
{
    dr_run_command(dry_run, "launchctl stop " SERVICE_NAME " 2>/dev/null");
    dr_run_command(dry_run, "launchctl unload " PLIST_PATH " 2>/dev/null");
}

int service_install(int dry_run)
{
    /* Root check: skipped in dry-run because no privileged operations run.
     * Lets CI / contributors smoke-test the install flow without sudo. */
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for installation\n");
        fprintf(stderr, "Usage: sudo %s --install\n", BINARY_PATH);
        return 1;
    }

    /* Non-destructive: validate the binary exists at BINARY_PATH before
     * printing anything else. Surfaces "you haven't copied the binary yet"
     * cleanly in both real and dry-run modes (in dry-run we still want this
     * check because it's the most likely operator mistake). */
    struct stat st;
    if (stat(BINARY_PATH, &st) != 0) {
        fprintf(stderr, "Error: binary not found at %s\n", BINARY_PATH);
        fprintf(stderr, "Copy the binary there first, then run --install\n");
        return 1;
    }

    if (dry_run) {
        printf("DRY RUN: --install (no filesystem or service changes will be made)\n");
    } else {
        printf("Installing macOS Guest Agent...\n");
    }

    stop_existing(dry_run);

    /* Create directories */
    dr_mkdir_p(dry_run, "/usr/local/bin", 0755);
    dr_mkdir_p(dry_run, "/usr/local/share", 0755);
    dr_mkdir_p(dry_run, SHARE_PATH, 0755);

    /* Write plist from embedded data */
    if (dry_run) {
        printf("DRY RUN: would install LaunchDaemon configuration to %s\n", PLIST_PATH);
    } else {
        printf("Installing LaunchDaemon configuration...\n");
    }
    if (dr_write_file(dry_run, PLIST_PATH, (const char *)plist_data,
                      plist_data_len, 0644) != 0) {
        fprintf(stderr, "Error: failed to write %s\n", PLIST_PATH);
        return 1;
    }

    /* Create log file (touch — open for append, immediately close) */
    if (dry_run) {
        printf("DRY RUN: would touch %s\n", LOG_PATH);
    } else {
        FILE *logfp = fopen(LOG_PATH, "a");
        if (logfp) fclose(logfp);
    }

    /* Install log rotation config (keeps 5 rotated copies, 1MB max each) */
    dr_mkdir_p(dry_run, "/etc/newsyslog.d", 0755);
    const char *logrotate =
        "# Log rotation for mac-guest-agent\n"
        "/var/log/mac-guest-agent.log    644  5  1024  *  J\n";
    dr_write_file(dry_run, "/etc/newsyslog.d/mac-guest-agent.conf",
                  logrotate, strlen(logrotate), 0644);

    /* Create fsfreeze hook directory */
    dr_mkdir_p(dry_run, "/etc/qemu/fsfreeze-hook.d", 0700);

    /* Load and start service */
    if (dry_run) {
        printf("DRY RUN: would start service\n");
    } else {
        printf("Starting service...\n");
    }
    if (dr_run_command(dry_run, "launchctl load " PLIST_PATH) != 0) {
        fprintf(stderr, "Error: failed to load service\n");
        return 1;
    }
    dr_run_command(dry_run, "launchctl start " SERVICE_NAME);

    if (dry_run) {
        printf("DRY RUN complete — no files modified.\n");
        printf("  Would have installed: %s\n", BINARY_PATH);
        printf("  Would have written:   %s\n", PLIST_PATH);
    } else {
        printf("macOS Guest Agent installed successfully.\n");
        printf("  Binary:  %s\n", BINARY_PATH);
        printf("  Config:  %s\n", PLIST_PATH);
        printf("  Log:     %s\n", LOG_PATH);
        printf("\nService commands:\n");
        printf("  Status:    sudo launchctl list %s\n", SERVICE_NAME);
        printf("  Log:       tail -f %s\n", LOG_PATH);
        printf("  Stop:      sudo launchctl stop %s\n", SERVICE_NAME);
        printf("  Start:     sudo launchctl start %s\n", SERVICE_NAME);
        printf("  Uninstall: sudo %s --uninstall\n", BINARY_PATH);
    }
    return 0;
}

int service_uninstall(int dry_run)
{
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for uninstallation\n");
        return 1;
    }

    if (dry_run) {
        printf("DRY RUN: --uninstall (no filesystem or service changes will be made)\n");
    } else {
        printf("Uninstalling macOS Guest Agent...\n");
    }

    stop_existing(dry_run);

    /* Remove files */
    const char *files[] = { BINARY_PATH, PLIST_PATH, NULL };
    for (int i = 0; files[i]; i++) {
        struct stat st;
        if (stat(files[i], &st) == 0) {
            if (dry_run) {
                printf("DRY RUN: would remove: %s\n", files[i]);
            } else {
                unlink(files[i]);
                printf("  Removed: %s\n", files[i]);
            }
        } else if (dry_run) {
            printf("DRY RUN: %s not present (would skip)\n", files[i]);
        }
    }

    /* Remove share directory */
    struct stat st;
    if (stat(SHARE_PATH, &st) == 0) {
        if (dry_run) {
            printf("DRY RUN: would run: rm -rf %s\n", SHARE_PATH);
        } else {
            run_command("rm -rf " SHARE_PATH);
            printf("  Removed: %s\n", SHARE_PATH);
        }
    }

    if (dry_run) {
        printf("DRY RUN complete — no files modified.\n");
    } else {
        printf("macOS Guest Agent uninstalled.\n");
        printf("  Log file retained at: %s\n", LOG_PATH);
    }
    return 0;
}

int service_update(const char *new_binary_path, int dry_run)
{
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for update\n");
        return 1;
    }

    /* Non-destructive validation always runs (in dry-run too) — these
     * surface "you passed the wrong path" before any side effect. */
    if (!new_binary_path || !*new_binary_path) {
        fprintf(stderr, "Error: provide path to new binary\n");
        fprintf(stderr, "Usage: sudo mac-guest-agent --update /path/to/new/binary\n");
        fprintf(stderr, "\nTo update from another machine:\n");
        fprintf(stderr, "  1. Download the new binary on a machine with internet\n");
        fprintf(stderr, "  2. scp mac-guest-agent user@vm-ip:/tmp/\n");
        fprintf(stderr, "  3. sudo mac-guest-agent --update /tmp/mac-guest-agent\n");
        return 1;
    }

    struct stat st;
    if (stat(new_binary_path, &st) != 0) {
        fprintf(stderr, "Error: file not found: %s\n", new_binary_path);
        return 1;
    }

    /* Verify it's actually an executable */
    if (!(st.st_mode & S_IXUSR)) {
        fprintf(stderr, "Error: file is not executable: %s\n", new_binary_path);
        fprintf(stderr, "Run: chmod +x %s\n", new_binary_path);
        return 1;
    }

    if (dry_run) {
        printf("DRY RUN: --update %s (no filesystem or service changes will be made)\n", new_binary_path);
    } else {
        printf("Updating macOS Guest Agent...\n");
    }

    /* Stop service */
    if (dry_run) {
        printf("DRY RUN: would stop service (launchctl stop/unload)\n");
    } else {
        printf("  Stopping service...\n");
    }
    stop_existing(dry_run);

    /* Backup current binary */
    if (stat(BINARY_PATH, &st) == 0) {
        char backup[512];
        snprintf(backup, sizeof(backup), "%s.backup", BINARY_PATH);
        if (dry_run) {
            printf("DRY RUN: would rename %s -> %s\n", BINARY_PATH, backup);
        } else {
            rename(BINARY_PATH, backup);
            printf("  Backed up current binary to %s\n", backup);
        }
    } else if (dry_run) {
        printf("DRY RUN: %s not present, would skip backup\n", BINARY_PATH);
    }

    /* Copy new binary — use execv, not shell, to prevent injection */
    if (dry_run) {
        printf("DRY RUN: would run: cp %s %s\n", new_binary_path, BINARY_PATH);
        printf("DRY RUN: would run: chmod 755 %s\n", BINARY_PATH);
        printf("DRY RUN: would run: %s -V (verify new binary launches)\n", BINARY_PATH);
        printf("DRY RUN: would reload service (launchctl load/start)\n");
        printf("DRY RUN complete — no files modified.\n");
        return 0;
    }

    char *const cp_argv[] = { "cp", (char *)new_binary_path, BINARY_PATH, NULL };
    char *const chmod_argv[] = { "chmod", "755", BINARY_PATH, NULL };
    if (run_command_v("cp", cp_argv, NULL, NULL) != 0 ||
        run_command_v("chmod", chmod_argv, NULL, NULL) != 0) {
        fprintf(stderr, "Error: failed to copy new binary\n");
        char backup[512];
        snprintf(backup, sizeof(backup), "%s.backup", BINARY_PATH);
        rename(backup, BINARY_PATH);
        return 1;
    }

    /* Verify new binary works */
    char *version_out = NULL;
    if (run_command_capture(BINARY_PATH " -V", &version_out) != 0 || !version_out) {
        fprintf(stderr, "Error: new binary failed to run\n");
        char backup[512];
        snprintf(backup, sizeof(backup), "%s.backup", BINARY_PATH);
        rename(backup, BINARY_PATH);
        free(version_out);
        return 1;
    }

    printf("  Installed: %s", version_out);
    free(version_out);

    /* Restart service */
    printf("  Restarting service...\n");
    run_command("launchctl load " PLIST_PATH " 2>/dev/null");
    run_command("launchctl start " SERVICE_NAME);

    printf("Update complete.\n");
    return 0;
}
