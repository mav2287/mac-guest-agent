#!/bin/bash
# macOS Guest Agent - Installation Script
# Supports macOS 10.4+ (i386 10.4+, x86_64 10.6+, arm64 11.0+)

set -e

REPO="mav2287/mac-guest-agent"
BINARY_NAME="mac-guest-agent"
INSTALL_PATH="/usr/local/bin/${BINARY_NAME}"

# --- VirtIO override constants (v2.5.3+) -------------------------------------
# These are referenced by --virtio, --virtio-force, and --uninstall. They are
# NOT used by the standard install path. See docs/NO_ISA_OVERRIDE.md for the
# operator-facing contract. The DIY path (--virtio-force) is undocumented on
# purpose — discoverable only through --help.
APPLE_AGENT_PLIST="/System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist"
APPLE_AGENT_LABEL="com.apple.AppleQEMUGuestAgent"
VIRTIO_DEVICE="/dev/cu.org.qemu.guest_agent.0"
VIRTIO_CONFIG_PATH="/etc/qemu/qemu-ga.conf"
VIRTIO_MARKER_DIR="/var/db/mac-guest-agent"
VIRTIO_MARKER_FILE="${VIRTIO_MARKER_DIR}/.virtio-mode"
OUR_DAEMON_LABEL="com.macos.guest-agent"
OUR_DAEMON_PLIST="/Library/LaunchDaemons/com.macos.guest-agent.plist"
AGENT_LOG_FILE="/var/log/mac-guest-agent.log"

info()  { echo "[INFO] $1" >&2; }
ok()    { echo "[OK]   $1" >&2; }
err()   { echo "[ERR]  $1" >&2; }
warn()  { echo "[WARN] $1" >&2; }

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        err "Root privileges required. Run with sudo."
        exit 1
    fi
}

# v2.5.0+: universal-only release. No per-arch detection for asset selection
# (the universal binary contains i386 + x86_64 + arm64 slices; dyld picks at
# load time). We still validate the host arch is one we ship a slice for, so
# unsupported architectures (e.g., PowerPC Tiger/Leopard) fail early with a
# clear message instead of downloading a universal binary dyld cannot load.
validate_arch() {
    case "$(uname -m)" in
        x86_64|i386|i486|i586|i686|arm64|arm64e) return 0 ;;
        *) err "Unsupported architecture: $(uname -m). This project ships i386, x86_64, and arm64 slices only. PowerPC and other architectures are not supported."; exit 1 ;;
    esac
}

stop_existing() {
    launchctl stop "$OUR_DAEMON_LABEL" 2>/dev/null || true
    launchctl unload "$OUR_DAEMON_PLIST" 2>/dev/null || true
}

check_serial_device() {
    # Check if the ISA serial device is available
    for dev in /dev/cu.serial1 /dev/cu.serial /dev/tty.serial1 /dev/tty.serial; do
        if [ -c "$dev" ]; then
            ok "Serial device found: $dev"
            return 0
        fi
    done
    return 1
}

# --- VirtIO override helpers (v2.5.3+) ----------------------------------------

# Major-version comparison. Returns 0 iff sw_vers reports productVersion with
# a leading major >= $1. Big Sur reports "11.x.y" so the major works as a
# proxy for "applevirtio.console is present in /System/Library/Extensions/".
check_macos_version_ge() {
    local target="$1"
    local major
    major=$(sw_vers -productVersion 2>/dev/null | awk -F. '{print $1}')
    if [ -z "$major" ]; then
        return 1
    fi
    [ "$major" -ge "$target" ]
}

# SIP must be off for `launchctl unload` against /System/Library/LaunchDaemons
# to succeed. csrutil reports either "System Integrity Protection status:
# disabled." (full disable) or per-category breakdown when a Custom
# Configuration is in effect — Filesystem Protections is the category that
# gates /System writes. Either form is acceptable.
check_sip_disabled() {
    local status_output
    status_output=$(csrutil status 2>/dev/null || true)
    case "$status_output" in
        *"System Integrity Protection status: disabled"*) return 0 ;;
        *"Filesystem Protections: disabled"*) return 0 ;;
    esac
    return 1
}

check_apple_agent_present() {
    [ -f "$APPLE_AGENT_PLIST" ]
}

check_virtio_device() {
    [ -c "$VIRTIO_DEVICE" ]
}

# Returns 0 if AppleQEMUGuestAgent is currently loaded in launchctl. Used to
# distinguish "already unloaded" (idempotent unload) from "unload failed."
apple_agent_loaded() {
    launchctl list 2>/dev/null | awk -v label="$APPLE_AGENT_LABEL" '$3==label {found=1} END {exit !found}'
}

# Returns the list of PIDs holding the VirtIO device, one per line, or empty
# if no holders. `lsof -t` is the portable form; macOS lsof supports it.
virtio_device_holders() {
    lsof -t "$VIRTIO_DEVICE" 2>/dev/null || true
}

# Probe whether our daemon is actually running (not just loaded). launchctl
# list reports PID as `-` for loaded-but-not-running entries; a numeric PID
# means there's a live process.
our_daemon_running_pid() {
    launchctl list 2>/dev/null | awk -v label="$OUR_DAEMON_LABEL" '$3==label && $1 ~ /^[0-9]+$/ {print $1; found=1} END {exit !found}'
}

# Check the agent log for evidence the agent opened the requested device.
# The log line format from src/channel.c:218 is: "Opened device: <path> (fd=...)".
agent_opened_virtio() {
    [ -f "$AGENT_LOG_FILE" ] || return 1
    grep -q "Opened device: $VIRTIO_DEVICE" "$AGENT_LOG_FILE"
}

write_virtio_config() {
    mkdir -p /etc/qemu
    chmod 755 /etc/qemu
    cat > "$VIRTIO_CONFIG_PATH" <<EOF
# Written by scripts/install.sh --virtio (mac-guest-agent v2.5.3+)
# This file overrides the default ISA-serial auto-detect path. It exists
# because this VM is running under an orchestrator that hardcodes VirtIO
# at the libvirt-channel level. See docs/NO_ISA_OVERRIDE.md.
# Removing this file restores ISA-serial auto-detect behavior.
[general]
path = $VIRTIO_DEVICE
EOF
    chmod 644 "$VIRTIO_CONFIG_PATH"
}

drop_marker() {
    mkdir -p "$VIRTIO_MARKER_DIR"
    chmod 700 "$VIRTIO_MARKER_DIR"
    printf 'mode=%s\n' "$1" > "$VIRTIO_MARKER_FILE"
    chmod 600 "$VIRTIO_MARKER_FILE"
}

read_marker_mode() {
    [ -f "$VIRTIO_MARKER_FILE" ] || return 1
    awk -F= '$1=="mode" {print $2; found=1} END {exit !found}' "$VIRTIO_MARKER_FILE"
}

remove_marker() {
    rm -f "$VIRTIO_MARKER_FILE"
    rmdir "$VIRTIO_MARKER_DIR" 2>/dev/null || true
}

# Read a single yes/no answer from /dev/tty (NOT stdin). Reading from
# /dev/tty means `yes | ./install.sh --virtio` cannot bypass the prompt —
# the prompt always talks to the user's terminal, not whatever's piped in.
# If no TTY is available (cron, headless CI, sshd without -t), we refuse to
# guess and bail. Returns 0 iff the user typed exactly "yes".
prompt_yes_no_from_tty() {
    if [ ! -r /dev/tty ] || [ ! -w /dev/tty ]; then
        err "No TTY available for confirmation. --virtio requires an interactive terminal. Run in an interactive shell."
        return 1
    fi
    local response
    printf 'Proceed? [yes/no]: ' > /dev/tty
    read -r response < /dev/tty
    [ "$response" = "yes" ]
}

print_virtio_warning() {
    cat >&2 <<'EOF'

================================================================
                  UNSUPPORTED CONFIGURATION
================================================================

You are installing mac-guest-agent with --virtio.

This will:
  - Unload Apple's built-in AppleQEMUGuestAgent
  - Switch mac-guest-agent to the VirtIO transport
  - Keep SIP disabled as a hard prerequisite

This is NOT a supported configuration. The supported transport
on macOS is ISA serial. Use this mode only if your orchestrator
hardcodes VirtIO and ISA isn't available.

Specifically:
  - This configuration is not covered by release-to-release
    stability promises.
  - macOS updates may re-enable AppleQEMUGuestAgent or change
    underlying behavior; you may need to reapply this install.
  - SIP being off reduces the kernel-integrity posture of this VM.

See docs/NO_ISA_OVERRIDE.md for the full contract.

EOF
}

# Unload Apple's daemon and verify it actually released the channel. The
# launchctl exit code is not trusted on its own — daemons can be stuck
# exiting, the LaunchDaemon may be re-spawned by IOKit-match before our
# next call, and the device fd may stay held briefly. We check both
# `launchctl list` (daemon gone) and `lsof` (device free) before declaring
# success. On failure, return non-zero so the caller can attempt rollback.
unload_apple_agent_and_verify() {
    info "Unloading Apple's AppleQEMUGuestAgent..."
    if ! launchctl unload -w "$APPLE_AGENT_PLIST" 2>/dev/null; then
        err "launchctl unload returned non-zero. Confirm SIP is disabled (csrutil status) and that the plist exists at $APPLE_AGENT_PLIST."
        return 1
    fi

    # Brief wait — the daemon needs time to exit and close its fds.
    sleep 1

    if apple_agent_loaded; then
        err "AppleQEMUGuestAgent is still listed in launchctl after unload. The unload did not land."
        return 2
    fi

    local holders
    holders=$(virtio_device_holders)
    if [ -n "$holders" ]; then
        local holder_info
        holder_info=$(lsof "$VIRTIO_DEVICE" 2>/dev/null | awk 'NR>1 {print $1"("$2")"}' | tr '\n' ',' | sed 's/,$//')
        err "VirtIO device $VIRTIO_DEVICE is still held by: $holder_info. Unload claimed success but the device fd is not released."
        return 3
    fi

    ok "AppleQEMUGuestAgent unloaded; VirtIO device released."
    return 0
}

# Best-effort restore Apple's daemon. Used during rollback when a later
# install step fails after the unload succeeded. Logs but does not fail —
# at this point we're already in a degraded state, and the caller has
# already decided to exit.
restore_apple_agent_best_effort() {
    info "Attempting to reload AppleQEMUGuestAgent to restore prior state..."
    if launchctl load -w "$APPLE_AGENT_PLIST" 2>/dev/null; then
        sleep 1
        if apple_agent_loaded; then
            ok "AppleQEMUGuestAgent reloaded."
            return 0
        fi
        warn "launchctl load returned 0 but daemon is not listed. Check 'launchctl list | grep $APPLE_AGENT_LABEL'."
        return 1
    fi
    warn "Failed to reload AppleQEMUGuestAgent. Restore manually with: sudo launchctl load -w $APPLE_AGENT_PLIST"
    return 1
}

# Wait up to ~5 seconds for the agent to come up and log that it opened the
# VirtIO device. The plist already has KeepAlive + RunAtLoad, so `launchctl
# load` (run by the binary's --install) starts it; we just need to give it
# a moment to actually connect to the channel.
verify_agent_on_virtio() {
    local pid=""
    local i=0
    while [ $i -lt 5 ]; do
        if pid=$(our_daemon_running_pid); then
            break
        fi
        sleep 1
        i=$((i+1))
    done
    if [ -z "$pid" ]; then
        err "Agent LaunchDaemon ($OUR_DAEMON_LABEL) did not start a process within 5 seconds."
        return 1
    fi
    info "Agent process running (PID $pid). Checking it opened the VirtIO device..."
    i=0
    while [ $i -lt 5 ]; do
        if agent_opened_virtio; then
            return 0
        fi
        sleep 1
        i=$((i+1))
    done
    err "Agent process started but the log does not show 'Opened device: $VIRTIO_DEVICE' within 5 seconds. Check $AGENT_LOG_FILE."
    return 1
}

# --- Install flows ------------------------------------------------------------

# Shared binary install step used by all three install flows. Assumes
# $BINARY is set and points at a valid universal binary.
install_binary() {
    stop_existing
    info "Installing binary..."
    mkdir -p /usr/local/bin
    cp "$BINARY" "$INSTALL_PATH"
    chmod +x "$INSTALL_PATH"

    info "Installing service..."
    "$INSTALL_PATH" --install
}

# --virtio: gated, safety-checked install path. Documented in
# docs/NO_ISA_OVERRIDE.md.
virtio_install() {
    info "Running --virtio prerequisite checks..."

    if ! check_macos_version_ge 11; then
        err "macOS $(sw_vers -productVersion 2>/dev/null || echo unknown) does not have the VirtIO console driver. The VirtIO override requires macOS 11 (Big Sur) or newer. Use ISA serial on this host — see docs/PVE.md / docs/LIBVIRT.md / docs/UTM.md."
        exit 1
    fi
    ok "macOS version check passed ($(sw_vers -productVersion))."

    if ! check_sip_disabled; then
        err "System Integrity Protection (SIP) must be disabled before --virtio has any effect. Boot to Recovery (Command-R during boot, or via the hypervisor's NVRAM reset), run 'csrutil disable', reboot, then retry this install. See docs/NO_ISA_OVERRIDE.md."
        exit 1
    fi
    ok "SIP check passed (disabled)."

    if ! check_apple_agent_present; then
        err "Apple's AppleQEMUGuestAgent LaunchDaemon is not present at $APPLE_AGENT_PLIST. There is nothing to override on this OS — use the standard install (no --virtio flag)."
        exit 1
    fi
    ok "AppleQEMUGuestAgent presence check passed."

    if ! check_virtio_device; then
        err "No VirtIO guest agent device found at $VIRTIO_DEVICE. The QEMU guest agent isn't enabled in your hypervisor configuration. Enable it, reboot the VM, and retry this install."
        exit 1
    fi
    ok "VirtIO device check passed ($VIRTIO_DEVICE)."

    print_virtio_warning

    if ! prompt_yes_no_from_tty; then
        info "Aborted by user. No changes made."
        exit 0
    fi

    # Past this point we start making changes. Each failure attempts
    # rollback to the prior state before exiting.
    if ! unload_apple_agent_and_verify; then
        # Nothing else changed yet, but if the unload partially succeeded
        # (e.g., process exited but launchctl record stuck), try to reload
        # so subsequent runs see a clean state.
        restore_apple_agent_best_effort
        exit 1
    fi

    install_binary

    info "Writing VirtIO override config to $VIRTIO_CONFIG_PATH..."
    write_virtio_config

    drop_marker "full"

    info "Restarting agent so it picks up the VirtIO config..."
    launchctl stop "$OUR_DAEMON_LABEL" 2>/dev/null || true
    sleep 1
    launchctl start "$OUR_DAEMON_LABEL" 2>/dev/null || true

    if ! verify_agent_on_virtio; then
        err "Agent failed to come up on the VirtIO channel. Rolling back."
        launchctl unload "$OUR_DAEMON_PLIST" 2>/dev/null || true
        rm -f "$VIRTIO_CONFIG_PATH"
        remove_marker
        restore_apple_agent_best_effort
        exit 1
    fi

    echo "" >&2
    ok "mac-guest-agent installed in VirtIO override mode."
    cat >&2 <<EOF

  Marker:    $VIRTIO_MARKER_FILE (mode=full)
  Config:    $VIRTIO_CONFIG_PATH
  Log:       $AGENT_LOG_FILE
  Status:    sudo launchctl list $OUR_DAEMON_LABEL
  Uninstall: sudo $0 --uninstall   (restores AppleQEMUGuestAgent)

EOF
}

# --virtio-force: bypass everything. Undocumented in operator-facing pages.
# For the case where the gated path's checks are wrong (false positives on
# SIP detection, Apple agent already manually unloaded, custom QEMU config
# with a different device path the operator will hand-edit, etc.).
virtio_force_install() {
    warn "--virtio-force enabled. All safety checks bypassed. Unsupported."

    install_binary

    info "Writing VirtIO override config to $VIRTIO_CONFIG_PATH..."
    write_virtio_config

    drop_marker "force"

    info "Restarting agent so it picks up the VirtIO config..."
    launchctl stop "$OUR_DAEMON_LABEL" 2>/dev/null || true
    sleep 1
    launchctl start "$OUR_DAEMON_LABEL" 2>/dev/null || true

    echo "" >&2
    ok "mac-guest-agent installed in VirtIO force mode (no safety checks performed)."
    cat >&2 <<EOF

  Marker:    $VIRTIO_MARKER_FILE (mode=force)
  Config:    $VIRTIO_CONFIG_PATH
  Log:       $AGENT_LOG_FILE
  Uninstall: sudo $0 --uninstall   (does NOT touch AppleQEMUGuestAgent)

EOF
}

# --uninstall: removes the agent. If a VirtIO marker is present, additionally
# removes the override config and (mode=full only) reloads Apple's daemon.
# Safe to run repeatedly — every step is idempotent.
uninstall_flow() {
    local mode=""
    if mode=$(read_marker_mode 2>/dev/null); then
        info "VirtIO override marker found (mode=$mode)."
    fi

    info "Stopping and removing mac-guest-agent..."
    if [ -x "$INSTALL_PATH" ]; then
        "$INSTALL_PATH" --uninstall 2>/dev/null || true
    else
        # Binary already gone; fall back to manual cleanup of LaunchDaemon
        # state so re-installs aren't blocked by a stale plist.
        launchctl stop "$OUR_DAEMON_LABEL" 2>/dev/null || true
        launchctl unload "$OUR_DAEMON_PLIST" 2>/dev/null || true
        rm -f "$OUR_DAEMON_PLIST"
    fi
    rm -f "$INSTALL_PATH"

    if [ -n "$mode" ]; then
        info "Removing VirtIO override config..."
        rm -f "$VIRTIO_CONFIG_PATH"

        if [ "$mode" = "full" ]; then
            restore_apple_agent_best_effort
        else
            info "VirtIO force mode: AppleQEMUGuestAgent state was not modified by us at install time, leaving it as-is."
        fi

        remove_marker
    fi

    ok "Uninstall complete."
    if [ -n "$mode" ]; then
        warn "SIP was not re-enabled by this script. To restore SIP, reboot to Recovery and run 'csrutil enable'."
    fi
}

main() {
    echo "=== macOS Guest Agent Installer ===" >&2

    # Phase 1: extract all known flags from the arg list, leaving --local
    # and its optional positional path intact for the parser below.
    DRY_RUN=0
    VIRTIO_MODE=""   # "", "virtio", or "force"
    UNINSTALL=0
    NEW_ARGS=()
    for arg in "$@"; do
        case "$arg" in
            --dry-run)
                DRY_RUN=1
                ;;
            --virtio)
                if [ -n "$VIRTIO_MODE" ]; then
                    err "Cannot combine --virtio and --virtio-force."
                    exit 1
                fi
                VIRTIO_MODE="virtio"
                ;;
            --virtio-force)
                if [ -n "$VIRTIO_MODE" ]; then
                    err "Cannot combine --virtio and --virtio-force."
                    exit 1
                fi
                VIRTIO_MODE="force"
                ;;
            --uninstall)
                UNINSTALL=1
                ;;
            *)
                NEW_ARGS+=("$arg")
                ;;
        esac
    done
    set -- "${NEW_ARGS[@]:-}"

    if [ "$UNINSTALL" -eq 1 ]; then
        if [ -n "$VIRTIO_MODE" ] || [ "$DRY_RUN" -eq 1 ]; then
            err "--uninstall does not combine with --virtio / --virtio-force / --dry-run."
            exit 1
        fi
        check_root
        uninstall_flow
        exit 0
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        info "DRY RUN: no filesystem or service changes will be made."
    fi

    # --local PATH resolution (same as pre-v2.5.3, just gated behind the
    # flag detection above).
    if [ "${1:-}" = "--local" ]; then
        if [ -n "${2:-}" ]; then
            if [ ! -f "$2" ]; then
                err "Local binary not found at: $2"
                exit 1
            fi
            BINARY="$2"
        elif [ -f "./${BINARY_NAME}" ]; then
            BINARY="./${BINARY_NAME}"
        elif [ -f "/tmp/${BINARY_NAME}" ]; then
            BINARY="/tmp/${BINARY_NAME}"
        elif [ -f "build/${BINARY_NAME}-universal" ]; then
            BINARY="build/${BINARY_NAME}-universal"
        elif [ -f "build/${BINARY_NAME}" ]; then
            BINARY="build/${BINARY_NAME}"
        elif [ -f "./${BINARY_NAME}-darwin-universal" ]; then
            BINARY="./${BINARY_NAME}-darwin-universal"
        elif [ -f "/tmp/${BINARY_NAME}-darwin-universal" ]; then
            BINARY="/tmp/${BINARY_NAME}-darwin-universal"
        else
            err "No local binary found."
            err "Searched: ./${BINARY_NAME}, /tmp/${BINARY_NAME}, build/${BINARY_NAME}-universal, build/${BINARY_NAME}, ./${BINARY_NAME}-darwin-universal, /tmp/${BINARY_NAME}-darwin-universal"
            err "Pass an explicit path: sudo $0 --local /path/to/${BINARY_NAME}"
            exit 1
        fi
        info "Using local binary: $BINARY"
    fi

    if [ "$DRY_RUN" -eq 0 ]; then
        check_root
    else
        info "DRY RUN: skipping root check (no privileged operations will run)."
    fi

    validate_arch
    info "Installing universal binary (covers i386 / x86_64 / arm64 — dyld picks at load time on $(uname -m))"
    info "macOS: $(sw_vers -productVersion 2>/dev/null || echo 'unknown')"

    if [ "${1:-}" != "--local" ]; then
        info "Downloading latest release..."
        if [ "$DRY_RUN" -eq 1 ]; then
            info "DRY RUN: would download ${BINARY_NAME} from GitHub releases."
            BINARY="<dry-run-pending-download>"
        else
            TMPDIR=$(mktemp -d)
            trap "rm -rf $TMPDIR" EXIT

            BINARY_FILE="${BINARY_NAME}"
            URL="https://github.com/${REPO}/releases/latest/download/${BINARY_FILE}"

            if command -v curl >/dev/null 2>&1; then
                curl -fsSL -o "$TMPDIR/$BINARY_FILE" "$URL" || { err "Download failed. On older macOS, download from another machine and use: sudo $0 --local"; exit 1; }
            elif command -v wget >/dev/null 2>&1; then
                wget -q -O "$TMPDIR/$BINARY_FILE" "$URL" || { err "Download failed"; exit 1; }
            else
                err "curl or wget required. Or download the binary manually and use: sudo $0 --local"
                exit 1
            fi

            BINARY="$TMPDIR/$BINARY_FILE"
            ok "Downloaded"
        fi
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "" >&2
        info "DRY RUN: would now do:"
        if [ "$VIRTIO_MODE" = "virtio" ]; then
            echo "    [prereq checks: macOS>=11, SIP off, AppleQEMUGuestAgent present, VirtIO device present]"
            echo "    [interactive warning + yes/no via /dev/tty]"
            echo "    launchctl unload -w $APPLE_AGENT_PLIST"
            echo "    [verify: launchctl list && lsof on $VIRTIO_DEVICE]"
        elif [ "$VIRTIO_MODE" = "force" ]; then
            echo "    [no prereq checks, no Apple agent unload]"
        fi
        echo "    launchctl stop $OUR_DAEMON_LABEL"
        echo "    launchctl unload $OUR_DAEMON_PLIST"
        echo "    mkdir -p /usr/local/bin"
        echo "    cp \"$BINARY\" \"$INSTALL_PATH\""
        echo "    chmod +x \"$INSTALL_PATH\""
        echo "    \"$INSTALL_PATH\" --install"
        if [ "$VIRTIO_MODE" = "virtio" ]; then
            echo "    write $VIRTIO_CONFIG_PATH with path = $VIRTIO_DEVICE"
            echo "    drop marker $VIRTIO_MARKER_FILE (mode=full)"
            echo "    launchctl stop/start $OUR_DAEMON_LABEL"
            echo "    [verify: agent running + log shows 'Opened device: $VIRTIO_DEVICE']"
        elif [ "$VIRTIO_MODE" = "force" ]; then
            echo "    write $VIRTIO_CONFIG_PATH with path = $VIRTIO_DEVICE"
            echo "    drop marker $VIRTIO_MARKER_FILE (mode=force)"
            echo "    launchctl stop/start $OUR_DAEMON_LABEL"
        else
            echo "    [serial-device probe]"
        fi
        info "DRY RUN complete — no files modified."
        exit 0
    fi

    # Hand off to the matching install flow.
    case "$VIRTIO_MODE" in
        virtio) virtio_install ;;
        force)  virtio_force_install ;;
        "")
            install_binary
            echo "" >&2
            if check_serial_device; then
                ok "Agent should connect automatically."
            else
                warn "No ISA serial device found."
                cat >&2 <<EOF

  The guest agent requires ISA serial mode on your hypervisor.
  On Proxmox VE, run this on the host:

    qm set <vmid> --agent enabled=1,type=isa

  Then restart the VM (stop + start, not reboot).
  The agent will connect automatically on next boot.
EOF
            fi

            echo "" >&2
            ok "macOS Guest Agent installed."
            cat >&2 <<EOF

  Status:    sudo launchctl list $OUR_DAEMON_LABEL
  Log:       tail -f $AGENT_LOG_FILE
  Uninstall: sudo $INSTALL_PATH --uninstall
EOF
            ;;
    esac
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    cat <<EOF
macOS Guest Agent Installer

Usage: sudo $0 [--local [PATH]] [--dry-run] [--virtio | --virtio-force]
       sudo $0 --uninstall

  --local         Install from a local binary (for VMs that can't reach GitHub).
                  Searches ./mac-guest-agent, /tmp/mac-guest-agent,
                  build/mac-guest-agent-universal, build/mac-guest-agent,
                  ./mac-guest-agent-darwin-universal,
                  /tmp/mac-guest-agent-darwin-universal (pre-v2.5.1 fallback).
  --local PATH    Install from an explicit binary path (skips the search).
  --dry-run       Print every action the installer would take WITHOUT touching
                  /usr/local/bin, /Library/LaunchDaemons, or launchctl. Argument
                  validation (--local PATH existence) still happens. Does not
                  require root. Combinable with --local, --virtio, --virtio-force.
  --virtio        Install with VirtIO transport override. Runs prerequisite
                  checks (macOS >= 11, SIP disabled, AppleQEMUGuestAgent
                  present, VirtIO device present), prints a risk block, and
                  requires interactive yes/no via /dev/tty before unloading
                  Apple's daemon and installing. See docs/NO_ISA_OVERRIDE.md
                  for the full contract and risks. Unsupported configuration.
  --virtio-force  Advanced: install with VirtIO path-override and skip all
                  safety checks. No SIP probe, no Apple agent unload, no
                  prompts. For experts who have already configured the host
                  manually. Unsupported.
  --uninstall     Remove mac-guest-agent. If installed via --virtio, also
                  removes /etc/qemu/qemu-ga.conf and reloads
                  AppleQEMUGuestAgent. If installed via --virtio-force,
                  removes our agent + config but does not touch Apple's
                  daemon. SIP is not re-enabled (operator action).
  (default)       Download latest release from GitHub and install with
                  ISA serial transport (supported).

Prerequisites for the standard install:
  On the Proxmox VE host, set ISA serial mode:
    qm set <vmid> --agent enabled=1,type=isa
  Then stop and start the VM.
EOF
    exit 0
fi

main "$@"
