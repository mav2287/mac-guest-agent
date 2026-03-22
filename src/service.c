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

static void stop_existing(void)
{
    run_command("launchctl stop " SERVICE_NAME " 2>/dev/null");
    run_command("launchctl unload " PLIST_PATH " 2>/dev/null");
}

int service_install(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for installation\n");
        fprintf(stderr, "Usage: sudo %s --install\n", BINARY_PATH);
        return 1;
    }

    printf("Installing macOS Guest Agent...\n");

    stop_existing();

    /* Check binary exists */
    struct stat st;
    if (stat(BINARY_PATH, &st) != 0) {
        fprintf(stderr, "Error: binary not found at %s\n", BINARY_PATH);
        fprintf(stderr, "Copy the binary there first, then run --install\n");
        return 1;
    }

    /* Create directories */
    mkdir_p("/usr/local/bin", 0755);
    mkdir_p("/usr/local/share", 0755);
    mkdir_p(SHARE_PATH, 0755);

    /* Write plist from embedded data */
    printf("Installing LaunchDaemon configuration...\n");
    if (write_file(PLIST_PATH, (const char *)plist_data, plist_data_len, 0644) != 0) {
        fprintf(stderr, "Error: failed to write %s\n", PLIST_PATH);
        return 1;
    }

    /* Create log file */
    FILE *logfp = fopen(LOG_PATH, "a");
    if (logfp) fclose(logfp);

    /* Install log rotation config (keeps 5 rotated copies, 1MB max each) */
    mkdir_p("/etc/newsyslog.d", 0755);
    const char *logrotate =
        "# Log rotation for mac-guest-agent\n"
        "/var/log/mac-guest-agent.log    644  5  1024  *  J\n";
    write_file("/etc/newsyslog.d/mac-guest-agent.conf",
               logrotate, strlen(logrotate), 0644);

    /* Create fsfreeze hook directory */
    mkdir_p("/etc/qemu/fsfreeze-hook.d", 0700);

    /* Load and start service */
    printf("Starting service...\n");
    if (run_command("launchctl load " PLIST_PATH) != 0) {
        fprintf(stderr, "Error: failed to load service\n");
        return 1;
    }
    run_command("launchctl start " SERVICE_NAME);

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
    return 0;
}

int service_uninstall(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for uninstallation\n");
        return 1;
    }

    printf("Uninstalling macOS Guest Agent...\n");

    stop_existing();

    /* Remove files */
    const char *files[] = { BINARY_PATH, PLIST_PATH, NULL };
    for (int i = 0; files[i]; i++) {
        struct stat st;
        if (stat(files[i], &st) == 0) {
            unlink(files[i]);
            printf("  Removed: %s\n", files[i]);
        }
    }

    /* Remove share directory */
    struct stat st;
    if (stat(SHARE_PATH, &st) == 0) {
        run_command("rm -rf " SHARE_PATH);
        printf("  Removed: %s\n", SHARE_PATH);
    }

    printf("macOS Guest Agent uninstalled.\n");
    printf("  Log file retained at: %s\n", LOG_PATH);
    return 0;
}

int service_update(const char *new_binary_path)
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for update\n");
        return 1;
    }

    if (!new_binary_path || !*new_binary_path) {
        fprintf(stderr, "Error: provide path to new binary\n");
        fprintf(stderr, "Usage: sudo mac-guest-agent --update /path/to/new/binary\n");
        fprintf(stderr, "\nTo update from another machine:\n");
        fprintf(stderr, "  1. Download the new binary on a machine with internet\n");
        fprintf(stderr, "  2. scp mac-guest-agent-darwin-amd64 user@vm-ip:/tmp/\n");
        fprintf(stderr, "  3. sudo mac-guest-agent --update /tmp/mac-guest-agent-darwin-amd64\n");
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

    printf("Updating macOS Guest Agent...\n");

    /* Stop service */
    printf("  Stopping service...\n");
    stop_existing();

    /* Backup current binary */
    if (stat(BINARY_PATH, &st) == 0) {
        char backup[512];
        snprintf(backup, sizeof(backup), "%s.backup", BINARY_PATH);
        rename(BINARY_PATH, backup);
        printf("  Backed up current binary to %s\n", backup);
    }

    /* Copy new binary — use execv, not shell, to prevent injection */
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
