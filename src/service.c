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
