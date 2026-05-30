#include "service.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>   /* _NSGetExecutablePath for self-source flows */

/* Embedded plist data - generated at build time by xxd */
#include "plist_data.h"

/* Resolve the canonical absolute path of the currently-running binary.
 * Writes the path into out_buf (which must be at least PATH_MAX bytes).
 * Returns 0 on success, -1 on failure.
 *
 * Used by service_install (to self-copy from /tmp to BINARY_PATH when run
 * outside the installed location) and service_upgrade (to use self as the
 * upgrade source when no explicit path is given). v2.5.3+. */
static int get_self_executable_path(char *out_buf, size_t buf_size)
{
    char raw[1024];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0)
        return -1;
    /* Resolve symlinks and ../-style components. realpath() is safe to call
     * with a NULL second arg on macOS 10.6+, but allocating a fixed buffer
     * makes the lifetime clearer. */
    char resolved[1024];
    if (realpath(raw, resolved) == NULL) {
        /* realpath failed — fall back to the raw path. Some edge cases
         * (binary deleted while running, weird mount layouts) can hit this
         * but we'd rather use the raw path than fail entirely. */
        snprintf(out_buf, buf_size, "%s", raw);
        return 0;
    }
    snprintf(out_buf, buf_size, "%s", resolved);
    return 0;
}

/* Helpers that gate each side-effect on the dry_run flag. Used by the
 * service_* handlers below. When dry_run is set, the helper prints
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

/* --- VirtIO override helpers (v2.5.3+) --------------------------------------
 *
 * These back the INSTALL_MODE_VIRTIO and INSTALL_MODE_VIRTIO_FORCE paths
 * through service_install, plus service_upgrade's mode-aware verify and
 * service_uninstall's marker-aware Apple-daemon restore. Test hooks (see
 * MAC_GUEST_AGENT_TEST_* env vars below) let the test suite exercise the
 * refusal paths without needing csrutil / launchctl / lsof on the host.
 *
 * The architecture deliberately mirrors what install.sh did in v2.5.3's
 * first attempt — same prereq surface, same warning text, same marker
 * format — so anyone who reviewed that work recognizes the shape here.
 * v2.5.3 final moved it into the binary because install.sh isn't surfaced
 * in the README and the second transfer for kubevirt operators was real
 * UX friction. See CHANGELOG v2.5.3 "Refactor".
 */

/* Test override hooks. When MAC_GUEST_AGENT_TEST_STATE is set, detect_install_state
 * returns the value verbatim ("not-installed" / "standard" / "virtio-full" /
 * "virtio-force"). When MAC_GUEST_AGENT_TEST_CONFIG_EXISTS is set,
 * operator_config_exists returns 1 iff value == "1". Tests use these to
 * exercise refusal paths without privileged setup. */
static const char *test_override_state(void)
{
    return getenv("MAC_GUEST_AGENT_TEST_STATE");
}

static int check_macos_version_ge(int target_major)
{
    /* Use sw_vers to honor SYSTEM_VERSION_COMPAT and other Apple
     * normalizations. The major version is "10" pre-Big Sur and 11/12/13/...
     * from Big Sur onward. */
    char *out = NULL;
    int rc = run_command_capture("sw_vers -productVersion 2>/dev/null", &out);
    if (rc != 0 || !out) {
        free(out);
        return 0;
    }
    int major = atoi(out);
    free(out);
    return major >= target_major;
}

static int check_sip_disabled(void)
{
    /* csrutil status outputs either:
     *   "System Integrity Protection status: disabled."  (full disable)
     * or per-category breakdown when a Custom Configuration is in effect:
     *   "Filesystem Protections: disabled" lines below the header
     * The second form is what `csrutil disable` produces on modern macOS
     * with selective per-category control. Either form means /System/Library
     * writes are permitted. */
    char *out = NULL;
    int rc = run_command_capture("csrutil status 2>/dev/null", &out);
    if (rc != 0 || !out) {
        free(out);
        return 0;
    }
    int disabled = 0;
    if (strstr(out, "System Integrity Protection status: disabled") != NULL)
        disabled = 1;
    else if (strstr(out, "Filesystem Protections: disabled") != NULL)
        disabled = 1;
    free(out);
    return disabled;
}

static int check_apple_agent_plist_present(void)
{
    struct stat st;
    return stat(APPLE_AGENT_PLIST, &st) == 0;
}

static int check_virtio_device_present(void)
{
    struct stat st;
    return stat(VIRTIO_DEVICE_PATH, &st) == 0 && S_ISCHR(st.st_mode);
}

/* Returns 1 if AppleQEMUGuestAgent is currently loaded in launchctl, 0
 * otherwise. Used to verify the unload landed. */
static int check_apple_agent_loaded(void)
{
    char *out = NULL;
    int rc = run_command_capture("launchctl list 2>/dev/null", &out);
    if (rc != 0 || !out) {
        free(out);
        return 0;
    }
    int found = (strstr(out, APPLE_AGENT_LABEL) != NULL) ? 1 : 0;
    free(out);
    return found;
}

/* Returns 1 if any process holds the VirtIO device open, 0 otherwise. */
static int check_virtio_device_held(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "lsof -t %s 2>/dev/null", VIRTIO_DEVICE_PATH);
    char *out = NULL;
    int rc = run_command_capture(cmd, &out);
    if (rc != 0 || !out) {
        free(out);
        return 0;
    }
    /* lsof -t prints one PID per line. Any non-whitespace content means
     * something holds the fd. */
    int held = 0;
    for (char *p = out; *p; p++) {
        if (*p != '\n' && *p != ' ' && *p != '\t' && *p != '\r') {
            held = 1;
            break;
        }
    }
    free(out);
    return held;
}

/* Returns 1 if our daemon has a numeric PID in launchctl list (i.e., is
 * actually running, not just loaded). 0 means loaded-but-not-running or
 * absent. */
static int check_our_daemon_running(void)
{
    char *out = NULL;
    int rc = run_command_capture("launchctl list 2>/dev/null", &out);
    if (rc != 0 || !out) {
        free(out);
        return 0;
    }
    int running = 0;
    char *line = strtok(out, "\n");
    while (line) {
        if (strstr(line, SERVICE_NAME) != NULL) {
            /* Format: "<PID> <Status> <Label>" — PID is "-" when not running. */
            if (line[0] >= '0' && line[0] <= '9')
                running = 1;
            break;
        }
        line = strtok(NULL, "\n");
    }
    free(out);
    return running;
}

/* Read a single yes/no answer from /dev/tty (NOT stdin). Reading from /dev/tty
 * means `yes | mac-guest-agent --install --virtio` cannot bypass the prompt.
 * Returns 1 iff the user typed exactly "yes". Returns 0 on any other input
 * or if no TTY is available. */
static int prompt_yes_no_from_tty(void)
{
    FILE *tty_in = fopen("/dev/tty", "r");
    FILE *tty_out = fopen("/dev/tty", "w");
    if (!tty_in || !tty_out) {
        fprintf(stderr, "Error: no TTY available for confirmation. --install --virtio "
                        "requires an interactive terminal. Run in an interactive shell.\n");
        if (tty_in) fclose(tty_in);
        if (tty_out) fclose(tty_out);
        return 0;
    }
    fprintf(tty_out, "Proceed? [yes/no]: ");
    fflush(tty_out);
    char buf[64];
    int got = (fgets(buf, sizeof(buf), tty_in) != NULL);
    fclose(tty_in);
    fclose(tty_out);
    if (!got) return 0;
    /* Strip trailing newline. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return strcmp(buf, "yes") == 0;
}

static void print_virtio_warning(void)
{
    fprintf(stderr,
        "\n"
        "================================================================\n"
        "                  UNSUPPORTED CONFIGURATION\n"
        "================================================================\n"
        "\n"
        "You are installing mac-guest-agent with --virtio.\n"
        "\n"
        "This will:\n"
        "  - Unload Apple's built-in AppleQEMUGuestAgent\n"
        "  - Switch mac-guest-agent to the VirtIO transport\n"
        "  - Keep SIP disabled as a hard prerequisite\n"
        "\n"
        "This is NOT a supported configuration. The supported transport\n"
        "on macOS is ISA serial. Use this mode only if your orchestrator\n"
        "hardcodes VirtIO and ISA isn't available.\n"
        "\n"
        "Specifically:\n"
        "  - This configuration is not covered by release-to-release\n"
        "    stability promises.\n"
        "  - macOS updates may re-enable AppleQEMUGuestAgent or change\n"
        "    underlying behavior; you may need to reapply this install.\n"
        "  - SIP being off reduces the kernel-integrity posture of this VM.\n"
        "\n"
        "See docs/NO_ISA_OVERRIDE.md for the full contract.\n"
        "\n");
}

static int drop_virtio_marker(const char *mode_str)
{
    if (mkdir_p(VIRTIO_MARKER_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    char line[64];
    int n = snprintf(line, sizeof(line), "mode=%s\n", mode_str);
    if (n < 0 || (size_t)n >= sizeof(line)) return -1;
    return write_file(VIRTIO_MARKER_FILE, line, (size_t)n, 0600);
}

/* Read the marker file. Returns: 0 if absent, 1 if mode=full, 2 if mode=force,
 * -1 on parse error. Caller passes a buffer for the literal mode string. */
static int read_virtio_marker(void)
{
    size_t len = 0;
    char *data = read_file(VIRTIO_MARKER_FILE, &len);
    if (!data) return 0;
    int result = -1;
    if (strstr(data, "mode=full") != NULL)
        result = 1;
    else if (strstr(data, "mode=force") != NULL)
        result = 2;
    free(data);
    return result;
}

static void remove_virtio_marker(void)
{
    unlink(VIRTIO_MARKER_FILE);
    rmdir(VIRTIO_MARKER_DIR);  /* fails harmlessly if non-empty or absent */
}

static int write_virtio_config(int dry_run)
{
    const char *body =
        "# Written by mac-guest-agent --install --virtio (v2.5.3+).\n"
        "# This file overrides the default ISA-serial auto-detect path.\n"
        "# It exists because this VM is running under an orchestrator that\n"
        "# hardcodes VirtIO at the libvirt-channel level. See\n"
        "# docs/NO_ISA_OVERRIDE.md. Removing this file restores ISA-serial\n"
        "# auto-detect behavior.\n"
        "[general]\n"
        "path = " VIRTIO_DEVICE_PATH "\n";
    if (dry_run) {
        printf("DRY RUN: would write %zu bytes to %s (mode 0644)\n",
               strlen(body), VIRTIO_CONFIG_PATH);
        return 0;
    }
    if (mkdir_p("/etc/qemu", 0755) != 0 && errno != EEXIST)
        return -1;
    return write_file(VIRTIO_CONFIG_PATH, body, strlen(body), 0644);
}

/* Capture the agent log's current byte size BEFORE the daemon restart so
 * the verify step only inspects bytes written by THIS run's daemon. Returns
 * 0 if the log doesn't exist yet. */
static long long agent_log_size_bytes(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) != 0) return 0;
    return (long long)st.st_size;
}

/* Returns 1 if the agent log contains "Opened device: <VIRTIO_DEVICE_PATH>"
 * past byte offset $offset. Used by the post-restart functional verify to
 * avoid false-positives on stale log entries from a prior install. */
static int agent_opened_virtio_since(long long offset)
{
    FILE *fp = fopen(LOG_PATH, "r");
    if (!fp) return 0;
    if (offset > 0) {
        if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
    }
    char marker[256];
    snprintf(marker, sizeof(marker), "Opened device: %s", VIRTIO_DEVICE_PATH);
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, marker) != NULL) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Unload Apple's daemon and verify the unload actually landed. Returns 0 on
 * success, non-zero on failure. */
static int unload_apple_agent_and_verify(void)
{
    fprintf(stderr, "[INFO] Unloading Apple's AppleQEMUGuestAgent...\n");
    int rc = run_command("launchctl unload -w " APPLE_AGENT_PLIST " 2>/dev/null");
    if (rc != 0) {
        fprintf(stderr, "Error: launchctl unload returned non-zero. "
                        "Confirm SIP is disabled (csrutil status) and that the plist "
                        "exists at %s.\n", APPLE_AGENT_PLIST);
        return 1;
    }
    /* Brief wait — daemon needs time to exit and close its fds. */
    sleep(1);
    if (check_apple_agent_loaded()) {
        fprintf(stderr, "Error: AppleQEMUGuestAgent is still listed in launchctl "
                        "after unload. The unload did not land.\n");
        return 2;
    }
    if (check_virtio_device_held()) {
        fprintf(stderr, "Error: VirtIO device %s is still held by another process "
                        "after unload. Unload claimed success but the device fd is "
                        "not released.\n", VIRTIO_DEVICE_PATH);
        return 3;
    }
    fprintf(stderr, "[OK]   AppleQEMUGuestAgent unloaded; VirtIO device released.\n");
    return 0;
}

static int restore_apple_agent_best_effort(void)
{
    fprintf(stderr, "[INFO] Attempting to reload AppleQEMUGuestAgent to restore prior state...\n");
    int rc = run_command("launchctl load -w " APPLE_AGENT_PLIST " 2>/dev/null");
    if (rc != 0) {
        fprintf(stderr, "[WARN] Failed to reload AppleQEMUGuestAgent. "
                        "Restore manually: sudo launchctl load -w %s\n", APPLE_AGENT_PLIST);
        return 1;
    }
    sleep(1);
    if (check_apple_agent_loaded()) {
        fprintf(stderr, "[OK]   AppleQEMUGuestAgent reloaded.\n");
        return 0;
    }
    fprintf(stderr, "[WARN] launchctl load returned 0 but daemon is not listed. "
                    "Check: launchctl list | grep %s\n", APPLE_AGENT_LABEL);
    return 1;
}

/* Wait up to 5 seconds for the agent to come up + open the VirtIO device.
 * $log_offset is the byte position captured BEFORE the restart so we only
 * scan bytes added since. Returns 0 on success, non-zero on timeout. */
static int verify_agent_on_virtio(long long log_offset)
{
    int i;
    for (i = 0; i < 5; i++) {
        if (check_our_daemon_running()) break;
        sleep(1);
    }
    if (!check_our_daemon_running()) {
        fprintf(stderr, "Error: agent LaunchDaemon (%s) did not start a process "
                        "within 5 seconds.\n", SERVICE_NAME);
        return 1;
    }
    fprintf(stderr, "[INFO] Agent process running. Checking it opened the VirtIO device...\n");
    for (i = 0; i < 5; i++) {
        if (agent_opened_virtio_since(log_offset)) return 0;
        sleep(1);
    }
    fprintf(stderr, "Error: agent process started but log does not show "
                    "'Opened device: %s' within 5 seconds (looked past byte %lld). "
                    "Check %s.\n", VIRTIO_DEVICE_PATH, log_offset, LOG_PATH);
    return 1;
}

/* --- Public API: detect_install_state, operator_config_exists -------------- */

install_state_t detect_install_state(void)
{
    const char *test_state = test_override_state();
    if (test_state) {
        if (strcmp(test_state, "not-installed") == 0) return INSTALL_STATE_NOT_INSTALLED;
        if (strcmp(test_state, "standard") == 0)      return INSTALL_STATE_STANDARD;
        if (strcmp(test_state, "virtio-full") == 0)   return INSTALL_STATE_VIRTIO_FULL;
        if (strcmp(test_state, "virtio-force") == 0)  return INSTALL_STATE_VIRTIO_FORCE;
        return INSTALL_STATE_NOT_INSTALLED;
    }
    /* Marker is the most authoritative signal. */
    int marker = read_virtio_marker();
    if (marker == 1) return INSTALL_STATE_VIRTIO_FULL;
    if (marker == 2) return INSTALL_STATE_VIRTIO_FORCE;
    /* No marker — fall back to binary/plist presence. */
    struct stat st;
    int binary_present = (stat(BINARY_PATH, &st) == 0);
    int plist_present  = (stat(PLIST_PATH, &st) == 0);
    if (binary_present || plist_present) return INSTALL_STATE_STANDARD;
    return INSTALL_STATE_NOT_INSTALLED;
}

int operator_config_exists(void)
{
    const char *test = getenv("MAC_GUEST_AGENT_TEST_CONFIG_EXISTS");
    if (test) return strcmp(test, "1") == 0;
    struct stat st;
    return stat(VIRTIO_CONFIG_PATH, &st) == 0;
}

/* --- service_install (mode-aware, v2.5.3+) -------------------------------- */

/* Internal: run the prereq checks for INSTALL_MODE_VIRTIO. Returns 0 if all
 * pass, non-zero if any fail (with a specific error message printed). */
static int virtio_prereq_checks(void)
{
    fprintf(stderr, "[INFO] Running --virtio prerequisite checks...\n");
    if (!check_macos_version_ge(11)) {
        fprintf(stderr,
            "Error: this macOS does not have the VirtIO console driver. The "
            "VirtIO override requires macOS 11 (Big Sur) or newer. Use ISA "
            "serial on this host — see docs/PVE.md, docs/LIBVIRT.md, or "
            "docs/UTM.md.\n");
        return 1;
    }
    fprintf(stderr, "[OK]   macOS version check passed.\n");
    if (!check_sip_disabled()) {
        fprintf(stderr,
            "Error: System Integrity Protection (SIP) must be disabled before "
            "--virtio has any effect. Boot to Recovery (Command-R during "
            "boot, or via the hypervisor's NVRAM reset), run 'csrutil "
            "disable', reboot, then retry this install. See "
            "docs/NO_ISA_OVERRIDE.md.\n");
        return 1;
    }
    fprintf(stderr, "[OK]   SIP check passed (disabled).\n");
    if (!check_apple_agent_plist_present()) {
        fprintf(stderr,
            "Error: Apple's AppleQEMUGuestAgent LaunchDaemon is not present at "
            "%s. There is nothing to override on this OS — use the standard "
            "install (no --virtio flag).\n", APPLE_AGENT_PLIST);
        return 1;
    }
    fprintf(stderr, "[OK]   AppleQEMUGuestAgent presence check passed.\n");
    if (!check_virtio_device_present()) {
        fprintf(stderr,
            "Error: no VirtIO guest agent device found at %s. The QEMU guest "
            "agent isn't enabled in your hypervisor configuration. Enable it, "
            "reboot the VM, and retry this install.\n", VIRTIO_DEVICE_PATH);
        return 1;
    }
    fprintf(stderr, "[OK]   VirtIO device check passed (%s).\n", VIRTIO_DEVICE_PATH);
    return 0;
}

/* Internal: the standard ISA-serial install body. Used directly by
 * INSTALL_MODE_STANDARD and also by INSTALL_MODE_VIRTIO / VIRTIO_FORCE
 * after the mode-specific preamble runs. */
static int do_standard_install_body(int dry_run)
{
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
    return 0;
}

int service_install(int dry_run, install_mode_t mode)
{
    /* Root check: skipped in dry-run because no privileged operations run. */
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for installation\n");
        fprintf(stderr, "Usage: sudo %s --install\n", BINARY_PATH);
        return 1;
    }

    /* Mode-specific refusal logic runs BEFORE the binary-existence check.
     * The refusals are about install-state / operator-config, not about
     * whether the binary has been copied yet — refusal-based exits should
     * fire predictably regardless of binary placement. */
    if (mode == INSTALL_MODE_VIRTIO || mode == INSTALL_MODE_VIRTIO_FORCE) {
        /* Refusal logic: --install --virtio[-force] is fresh-install-only.
         * Existing install or operator config means refuse with a remediation
         * pointer. The check fires regardless of dry-run (we want the refusal
         * surfaced consistently). */
        install_state_t state = detect_install_state();
        if (state != INSTALL_STATE_NOT_INSTALLED) {
            fprintf(stderr,
                "Error: existing install detected. --install --virtio / --virtio-force "
                "is for fresh installs only.\n"
                "  To update in place:        sudo %s --upgrade /path/to/new/binary\n"
                "  To switch modes:           sudo %s --uninstall, then sudo %s --install --virtio\n",
                BINARY_PATH, BINARY_PATH, BINARY_PATH);
            return 1;
        }
        if (operator_config_exists()) {
            fprintf(stderr,
                "Error: pre-existing %s detected (operator state). --virtio "
                "refuses to overwrite it.\n"
                "\n"
                "  Recommended: back up the existing config, then re-run --virtio:\n"
                "      sudo cp %s %s.bak\n"
                "      sudo rm %s\n"
                "      sudo %s --install --virtio\n"
                "\n"
                "  After install, restore your customizations (allow-rpcs, block-rpcs,\n"
                "  etc.) to the new %s and bounce the LaunchDaemon.\n"
                "\n"
                "  DIY path (advanced; you take responsibility for the gates this skips):\n"
                "      sudo %s --install --virtio-force\n",
                VIRTIO_CONFIG_PATH, VIRTIO_CONFIG_PATH, VIRTIO_CONFIG_PATH,
                VIRTIO_CONFIG_PATH, BINARY_PATH, VIRTIO_CONFIG_PATH, BINARY_PATH);
            return 1;
        }
    }

    if (mode == INSTALL_MODE_VIRTIO) {
        if (virtio_prereq_checks() != 0)
            return 1;
        if (!dry_run) {
            print_virtio_warning();
            if (!prompt_yes_no_from_tty()) {
                fprintf(stderr, "[INFO] Aborted by user. No changes made.\n");
                return 0;
            }
        } else {
            fprintf(stderr, "DRY RUN: would print warning + prompt for yes/no via /dev/tty\n");
        }
    } else if (mode == INSTALL_MODE_VIRTIO_FORCE) {
        fprintf(stderr,
            "[WARN] --install --virtio-force enabled. All safety checks bypassed. "
            "Unsupported.\n");
    }

    /* Binary placement at BINARY_PATH. The plist's ProgramArguments
     * reference BINARY_PATH, so the binary must be there before --install
     * registers the LaunchDaemon.
     *
     * v2.5.3+: self-source. If the operator runs this binary from a
     * non-BINARY_PATH location (e.g., `sudo /tmp/mac-guest-agent --install`),
     * copy ourselves to BINARY_PATH first. Operators no longer have to do
     * `mv binary && run binary --install` as two steps — `run binary
     * --install` from anywhere just works.
     *
     * Test hook: skip the copy in dry-run when the test override is set
     * so the test suite can exercise the install plan without a real
     * /usr/local/bin/ entry. */
    if (!dry_run || !test_override_state()) {
        char self_path[1024];
        if (get_self_executable_path(self_path, sizeof(self_path)) != 0) {
            fprintf(stderr, "Error: could not resolve own executable path\n");
            return 1;
        }
        if (strcmp(self_path, BINARY_PATH) != 0) {
            if (dry_run) {
                printf("DRY RUN: would self-copy %s -> %s\n", self_path, BINARY_PATH);
            } else {
                if (mkdir_p("/usr/local/bin", 0755) != 0 && errno != EEXIST) {
                    fprintf(stderr, "Error: failed to create /usr/local/bin: %s\n", strerror(errno));
                    return 1;
                }
                char *const cp_argv[] = { "cp", self_path, BINARY_PATH, NULL };
                char *const chmod_argv[] = { "chmod", "755", BINARY_PATH, NULL };
                if (run_command_v("cp", cp_argv, NULL, NULL) != 0 ||
                    run_command_v("chmod", chmod_argv, NULL, NULL) != 0) {
                    fprintf(stderr, "Error: failed to copy %s -> %s\n", self_path, BINARY_PATH);
                    return 1;
                }
                printf("Self-installed binary: %s -> %s\n", self_path, BINARY_PATH);
            }
        } else {
            /* Already at BINARY_PATH; just confirm it still exists. */
            struct stat st;
            if (stat(BINARY_PATH, &st) != 0) {
                fprintf(stderr, "Error: binary not found at %s (and self path %s "
                                "is supposedly the same)\n", BINARY_PATH, self_path);
                return 1;
            }
        }
    }

    if (dry_run) {
        printf("DRY RUN: --install (mode=%s) — no filesystem or service changes will be made\n",
               mode == INSTALL_MODE_VIRTIO ? "virtio" :
               mode == INSTALL_MODE_VIRTIO_FORCE ? "virtio-force" : "standard");
    } else {
        printf("Installing macOS Guest Agent...\n");
    }

    /* For VIRTIO mode, unload Apple's daemon BEFORE the standard install
     * body runs. VIRTIO_FORCE skips this (operator did it manually). */
    if (mode == INSTALL_MODE_VIRTIO && !dry_run) {
        if (unload_apple_agent_and_verify() != 0) {
            restore_apple_agent_best_effort();
            return 1;
        }
    } else if (mode == INSTALL_MODE_VIRTIO && dry_run) {
        printf("DRY RUN: would launchctl unload -w %s\n", APPLE_AGENT_PLIST);
        printf("DRY RUN: would verify unload via launchctl list + lsof on %s\n",
               VIRTIO_DEVICE_PATH);
    }

    /* Standard install body — same for all modes (writes plist, log,
     * rotation config, hook dir, loads + starts service). */
    if (do_standard_install_body(dry_run) != 0) {
        /* On install failure during VIRTIO mode, attempt to restore Apple's
         * daemon so the host isn't left without an agent. */
        if (mode == INSTALL_MODE_VIRTIO && !dry_run) {
            restore_apple_agent_best_effort();
        }
        return 1;
    }

    /* For VIRTIO and VIRTIO_FORCE: write override config + drop marker +
     * functional verify. */
    if (mode == INSTALL_MODE_VIRTIO || mode == INSTALL_MODE_VIRTIO_FORCE) {
        if (write_virtio_config(dry_run) != 0) {
            fprintf(stderr, "Error: failed to write %s\n", VIRTIO_CONFIG_PATH);
            if (!dry_run && mode == INSTALL_MODE_VIRTIO) {
                restore_apple_agent_best_effort();
            }
            return 1;
        }
        if (!dry_run) {
            if (drop_virtio_marker(mode == INSTALL_MODE_VIRTIO ? "full" : "force") != 0) {
                fprintf(stderr, "[WARN] failed to drop marker file %s\n",
                        VIRTIO_MARKER_FILE);
            }
        } else {
            printf("DRY RUN: would drop marker %s (mode=%s)\n",
                   VIRTIO_MARKER_FILE,
                   mode == INSTALL_MODE_VIRTIO ? "full" : "force");
        }

        /* Restart agent so it picks up the override config. */
        if (!dry_run) {
            long long log_offset = agent_log_size_bytes();
            run_command("launchctl stop " SERVICE_NAME " 2>/dev/null");
            sleep(1);
            run_command("launchctl start " SERVICE_NAME);
            /* Verify only in --virtio mode; --virtio-force trusts the operator. */
            if (mode == INSTALL_MODE_VIRTIO) {
                if (verify_agent_on_virtio(log_offset) != 0) {
                    fprintf(stderr, "[ERR]  Agent failed to come up on VirtIO channel. Rolling back.\n");
                    run_command("launchctl unload " PLIST_PATH " 2>/dev/null");
                    unlink(PLIST_PATH);
                    unlink(BINARY_PATH);
                    unlink(VIRTIO_CONFIG_PATH);
                    remove_virtio_marker();
                    restore_apple_agent_best_effort();
                    return 1;
                }
            }
        } else {
            printf("DRY RUN: would restart %s and verify agent opened %s\n",
                   SERVICE_NAME, VIRTIO_DEVICE_PATH);
        }
    }

    if (dry_run) {
        printf("DRY RUN complete — no files modified.\n");
        return 0;
    }

    if (mode == INSTALL_MODE_VIRTIO || mode == INSTALL_MODE_VIRTIO_FORCE) {
        printf("\nmac-guest-agent installed in VirtIO override mode (%s).\n",
               mode == INSTALL_MODE_VIRTIO ? "managed" : "force");
        printf("  Marker:    %s (mode=%s)\n",
               VIRTIO_MARKER_FILE,
               mode == INSTALL_MODE_VIRTIO ? "full" : "force");
        printf("  Config:    %s\n", VIRTIO_CONFIG_PATH);
        printf("  Log:       %s\n", LOG_PATH);
        printf("  Status:    sudo launchctl list %s\n", SERVICE_NAME);
        printf("  Uninstall: sudo %s --uninstall%s\n", BINARY_PATH,
               mode == INSTALL_MODE_VIRTIO ? "   (restores AppleQEMUGuestAgent)"
                                           : "   (does NOT touch AppleQEMUGuestAgent)");
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

/* --- service_uninstall (marker-aware) ------------------------------------- */

int service_uninstall(int dry_run)
{
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for uninstallation\n");
        return 1;
    }

    int marker = read_virtio_marker();   /* 0=none, 1=full, 2=force */

    if (dry_run) {
        printf("DRY RUN: --uninstall (no filesystem or service changes will be made)\n");
        if (marker > 0) {
            printf("DRY RUN: VirtIO marker detected (mode=%s)\n",
                   marker == 1 ? "full" : "force");
        }
    } else {
        printf("Uninstalling macOS Guest Agent...\n");
        if (marker > 0) {
            printf("  VirtIO marker detected (mode=%s)\n", marker == 1 ? "full" : "force");
        }
    }

    stop_existing(dry_run);

    /* Remove files. Add the override config when a marker is present. */
    const char *files_standard[]    = { BINARY_PATH, PLIST_PATH, NULL };
    const char *files_with_marker[] = { BINARY_PATH, PLIST_PATH, VIRTIO_CONFIG_PATH, NULL };
    const char **files = (marker > 0) ? files_with_marker : files_standard;
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

    /* Marker-aware Apple-daemon restore. mode=full means WE unloaded the
     * daemon at install time, so reload it. mode=force means the operator
     * unloaded it manually, so leave it alone. */
    if (marker == 1) {
        if (dry_run) {
            printf("DRY RUN: would launchctl load -w %s (mode=full restore)\n",
                   APPLE_AGENT_PLIST);
        } else {
            restore_apple_agent_best_effort();
        }
    } else if (marker == 2) {
        if (dry_run) {
            printf("DRY RUN: mode=force — would NOT touch AppleQEMUGuestAgent\n");
        } else {
            printf("  VirtIO force mode: AppleQEMUGuestAgent state was not modified by us at install; leaving it as-is.\n");
        }
    }

    /* Remove marker last so it's still present during the Apple-daemon
     * restore decision above. */
    if (marker > 0) {
        if (dry_run) {
            printf("DRY RUN: would remove marker %s\n", VIRTIO_MARKER_FILE);
        } else {
            remove_virtio_marker();
        }
    }

    if (dry_run) {
        printf("DRY RUN complete — no files modified.\n");
    } else {
        printf("macOS Guest Agent uninstalled.\n");
        printf("  Log file retained at: %s\n", LOG_PATH);
        if (marker > 0) {
            printf("  WARN: SIP was not re-enabled by this uninstall. To restore SIP, "
                   "reboot to Recovery and run 'csrutil enable'.\n");
        }
    }
    return 0;
}

/* --- service_upgrade (v2.5.3+ proper upgrade flow) ------------------------ */

int service_upgrade(const char *new_binary_path, int dry_run)
{
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required for upgrade\n");
        return 1;
    }

    /* v2.5.3+: self-source. When new_binary_path is NULL (the operator
     * ran `mac-guest-agent --upgrade` without a path argument), use the
     * currently-running binary as the source. Operators with the new
     * binary at /tmp/ just run `sudo /tmp/mac-guest-agent --upgrade`
     * instead of typing the path twice. */
    char self_path_buf[1024];
    if (!new_binary_path || !*new_binary_path) {
        if (get_self_executable_path(self_path_buf, sizeof(self_path_buf)) != 0) {
            fprintf(stderr, "Error: --upgrade with no path: could not resolve "
                            "own executable path. Pass a path explicitly: "
                            "sudo %s --upgrade /path/to/new/binary (via --update PATH "
                            "during the deprecation window).\n", BINARY_PATH);
            return 1;
        }
        new_binary_path = self_path_buf;
        fprintf(stderr, "[INFO] --upgrade using self as source: %s\n", new_binary_path);
    }

    install_state_t state = detect_install_state();
    if (state == INSTALL_STATE_NOT_INSTALLED) {
        fprintf(stderr,
            "Error: --upgrade requested but no existing install detected. "
            "Run sudo %s --install for a fresh install.\n", BINARY_PATH);
        return 1;
    }

    /* Guard against the degenerate case where self IS the installed binary
     * (operator ran `sudo /usr/local/bin/mac-guest-agent --upgrade`). We
     * can't cp a file to itself, and there's no sensible meaning to
     * "upgrade the installed binary using itself as source" — refuse
     * with a clear pointer. */
    if (strcmp(new_binary_path, BINARY_PATH) == 0) {
        fprintf(stderr,
            "Error: --upgrade source and destination are the same (%s). "
            "Run the NEW binary, not the already-installed one:\n"
            "  sudo /path/to/new/mac-guest-agent --upgrade\n", BINARY_PATH);
        return 1;
    }

    struct stat st;
    if (stat(new_binary_path, &st) != 0) {
        fprintf(stderr, "Error: file not found: %s\n", new_binary_path);
        return 1;
    }
    if (!(st.st_mode & S_IXUSR)) {
        fprintf(stderr, "Error: file is not executable: %s\n", new_binary_path);
        fprintf(stderr, "Run: chmod +x %s\n", new_binary_path);
        return 1;
    }

    if (dry_run) {
        printf("DRY RUN: --upgrade %s (detected state=%s)\n", new_binary_path,
               state == INSTALL_STATE_STANDARD ? "standard" :
               state == INSTALL_STATE_VIRTIO_FULL ? "virtio-full" : "virtio-force");
        printf("DRY RUN: would backup %s -> %s.backup\n", BINARY_PATH, BINARY_PATH);
        printf("DRY RUN: would stop daemon, cp new binary, run --install, restart, verify\n");
        printf("DRY RUN complete — no files modified.\n");
        return 0;
    }

    /* Backup current binary so we can restore on rollback. cp not mv —
     * old binary stays runnable until the new one is verified. */
    char backup[512];
    snprintf(backup, sizeof(backup), "%s.backup", BINARY_PATH);
    char *const cp_backup_argv[] = { "cp", BINARY_PATH, backup, NULL };
    if (run_command_v("cp", cp_backup_argv, NULL, NULL) != 0) {
        fprintf(stderr, "Error: failed to back up current binary to %s\n", backup);
        return 1;
    }
    char *const chmod_backup_argv[] = { "chmod", "+x", backup, NULL };
    run_command_v("chmod", chmod_backup_argv, NULL, NULL);
    printf("Backed up current binary to %s\n", backup);

    stop_existing(0);

    printf("Installing new binary...\n");
    char *const cp_argv[] = { "cp", (char *)new_binary_path, BINARY_PATH, NULL };
    char *const chmod_argv[] = { "chmod", "755", BINARY_PATH, NULL };
    if (run_command_v("cp", cp_argv, NULL, NULL) != 0 ||
        run_command_v("chmod", chmod_argv, NULL, NULL) != 0) {
        fprintf(stderr, "Error: failed to copy new binary; restoring backup\n");
        rename(backup, BINARY_PATH);
        return 1;
    }

    /* Regenerate the LaunchDaemon plist by re-running the standard install
     * body. This is the crucial difference from the legacy --update path:
     * plist changes between releases actually land. We call
     * do_standard_install_body directly (not service_install) to skip the
     * self-copy logic (we already cp'd above) and the VIRTIO refusal/prereq
     * paths (the override config + marker are already in place from the
     * original install and stay untouched). */
    printf("Refreshing LaunchDaemon plist...\n");
    if (do_standard_install_body(0) != 0) {
        fprintf(stderr, "Error: plist regeneration failed; rolling back to backup\n");
        rename(backup, BINARY_PATH);
        run_command_v("chmod", (char *const[]){ "chmod", "755", BINARY_PATH, NULL }, NULL, NULL);
        do_standard_install_body(0);
        return 1;
    }

    /* Mode-aware verify. */
    if (state == INSTALL_STATE_VIRTIO_FULL || state == INSTALL_STATE_VIRTIO_FORCE) {
        long long log_offset = agent_log_size_bytes();
        run_command("launchctl stop " SERVICE_NAME " 2>/dev/null");
        sleep(1);
        run_command("launchctl start " SERVICE_NAME);
        if (verify_agent_on_virtio(log_offset) != 0) {
            fprintf(stderr, "Error: upgrade verify failed — agent did not open VirtIO "
                            "device. Rolling back to backup binary.\n");
            rename(backup, BINARY_PATH);
            run_command_v("chmod", (char *const[]){ "chmod", "755", BINARY_PATH, NULL }, NULL, NULL);
            do_standard_install_body(0);
            return 1;
        }
    } else {
        sleep(1);
        if (!check_our_daemon_running()) {
            fprintf(stderr, "Error: upgrade verify failed — daemon did not start within "
                            "1 second. Rolling back to backup binary.\n");
            rename(backup, BINARY_PATH);
            run_command_v("chmod", (char *const[]){ "chmod", "755", BINARY_PATH, NULL }, NULL, NULL);
            do_standard_install_body(0);
            return 1;
        }
    }

    /* Success. Remove backup. */
    unlink(backup);

    char *version_out = NULL;
    run_command_capture(BINARY_PATH " -V", &version_out);
    printf("\nUpgrade complete (state=%s).\n",
           state == INSTALL_STATE_STANDARD ? "standard" :
           state == INSTALL_STATE_VIRTIO_FULL ? "virtio-full" : "virtio-force");
    if (version_out) {
        printf("  Installed: %s", version_out);
        free(version_out);
    }
    printf("  Status:    sudo launchctl list %s\n", SERVICE_NAME);
    return 0;
}

/* --- service_update (DEPRECATED in v2.5.3, delegates to service_upgrade) -- */

int service_update(const char *new_binary_path, int dry_run)
{
    fprintf(stderr,
        "Notice: mac-guest-agent --update is deprecated in v2.5.3+.\n"
        "        Prefer: sudo %s --upgrade %s\n"
        "        --update now delegates to --upgrade internally; behavior is\n"
        "        equivalent. A future release will remove --update entirely.\n\n",
        BINARY_PATH,
        new_binary_path ? new_binary_path : "/path/to/new/binary");
    return service_upgrade(new_binary_path, dry_run);
}
