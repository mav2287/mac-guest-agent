#ifndef MGA_SERVICE_H
#define MGA_SERVICE_H

#define SERVICE_NAME    "com.macos.guest-agent"
#define BINARY_PATH     "/usr/local/bin/mac-guest-agent"
#define PLIST_PATH      "/Library/LaunchDaemons/com.macos.guest-agent.plist"
#define LOG_PATH        "/var/log/mac-guest-agent.log"
#define SHARE_PATH      "/usr/local/share/mac-guest-agent"

/* v2.5.3+: VirtIO override constants. The marker file is dropped by
 * service_install when called in VIRTIO or VIRTIO_FORCE mode and consulted
 * by service_uninstall to decide whether to reload Apple's daemon.
 * /etc/qemu/qemu-ga.conf is written with `path = $VIRTIO_DEVICE_PATH` so
 * the agent opens the VirtIO device instead of auto-detecting an ISA UART. */
#define APPLE_AGENT_PLIST   "/System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist"
#define APPLE_AGENT_LABEL   "com.apple.AppleQEMUGuestAgent"
#define VIRTIO_DEVICE_PATH  "/dev/cu.org.qemu.guest_agent.0"
#define VIRTIO_CONFIG_PATH  "/etc/qemu/qemu-ga.conf"
#define VIRTIO_MARKER_DIR   "/var/db/mac-guest-agent"
#define VIRTIO_MARKER_FILE  "/var/db/mac-guest-agent/.virtio-mode"

/* Install mode. STANDARD = ISA serial auto-detect (the documented and
 * supported path). VIRTIO = gated override (runs prereq checks, prompts for
 * confirmation, unloads Apple's daemon, drops marker). VIRTIO_FORCE = same
 * config write + marker as VIRTIO but no prereq checks, no Apple unload,
 * no prompt — for operators who've already configured the host manually. */
typedef enum {
    INSTALL_MODE_STANDARD = 0,
    INSTALL_MODE_VIRTIO,
    INSTALL_MODE_VIRTIO_FORCE
} install_mode_t;

/* dry_run: When set, prints every action the function WOULD take but does
 * not touch the filesystem or call launchctl. Root is also skipped in
 * dry-run mode. Used by install.sh --dry-run for end-to-end smoke testing. */
int service_install(int dry_run, install_mode_t mode);
int service_uninstall(int dry_run);

/* Update from a local binary file. v2.5.3+: DEPRECATED in favor of
 * service_upgrade. Kept for backward compat; prints a deprecation notice. */
int service_update(const char *new_binary_path, int dry_run);

/* v2.5.3+: proper in-place upgrade. Detects current install state (via
 * marker file + binary/plist presence), backs up current binary, copies
 * the new one, regenerates the plist via --install, restarts, verifies.
 * On verify failure restores the backup and re-runs its --install to
 * regenerate the plist for the old binary. Mode-aware: VirtIO installs
 * get the VirtIO functional verify (agent opened device); standard
 * installs get a process-running check. */
int service_upgrade(const char *new_binary_path, int dry_run);

/* v2.5.3+: detect current install state. Used by --upgrade to choose the
 * right verify, and by install.sh to refuse incompatible flag combinations
 * (e.g., --install --virtio when an existing install is detected). */
typedef enum {
    INSTALL_STATE_NOT_INSTALLED = 0,
    INSTALL_STATE_STANDARD,
    INSTALL_STATE_VIRTIO_FULL,
    INSTALL_STATE_VIRTIO_FORCE
} install_state_t;

install_state_t detect_install_state(void);

/* v2.5.3+: TRUE iff /etc/qemu/qemu-ga.conf exists. Used to refuse a fresh
 * VirtIO install that would clobber an operator's pre-customized config. */
int operator_config_exists(void);

#endif
