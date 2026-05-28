#!/bin/bash
# macOS Guest Agent - Installation Script
# Supports macOS 10.4+ (i386 10.4+, x86_64 10.6+, arm64 11.0+)

set -e

REPO="mav2287/mac-guest-agent"
BINARY_NAME="mac-guest-agent"
INSTALL_PATH="/usr/local/bin/${BINARY_NAME}"

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
    launchctl stop com.macos.guest-agent 2>/dev/null || true
    launchctl unload /Library/LaunchDaemons/com.macos.guest-agent.plist 2>/dev/null || true
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

main() {
    echo "=== macOS Guest Agent Installer ==="

    # Detect --dry-run and remove it from the argument list so the rest of
    # the parser keeps its positional layout (--local PATH still works).
    # --dry-run prints every action the installer WOULD take without
    # touching /usr/local/bin, /Library/LaunchDaemons, or launchctl.
    # Lets CI and audit-style smoke tests exercise the install path
    # without root and without modifying the host. Audit wave 5 LOW-3.
    DRY_RUN=0
    NEW_ARGS=()
    for arg in "$@"; do
        if [ "$arg" = "--dry-run" ]; then
            DRY_RUN=1
        else
            NEW_ARGS+=("$arg")
        fi
    done
    set -- "${NEW_ARGS[@]}"
    if [ "$DRY_RUN" -eq 1 ]; then
        info "DRY RUN: no filesystem or service changes will be made."
    fi

    # Resolve the local-binary argument BEFORE the root check so a bogus
    # --local PATH gives a clean "file not found" without prompting for
    # sudo first. Same for --local with no path falling through to the
    # search list. Audit wave 5 LOW-3.
    if [ "$1" = "--local" ]; then
        # Optional explicit path: `--local /path/to/binary` skips the search
        # entirely. Use this when the file was transferred to a non-standard
        # location or renamed.
        if [ -n "$2" ]; then
            if [ ! -f "$2" ]; then
                err "Local binary not found at: $2"
                exit 1
            fi
            BINARY="$2"
        # Search order: release asset name first (v2.5.1+ ships as plain
        # `mac-guest-agent` — what users actually transfer from a modern
        # machine), then in-tree build outputs, then the pre-v2.5.1
        # `-darwin-universal` name as recovery fallback for anyone whose
        # local copy is from the one-day v2.5.0 window.
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

    # Now that argument resolution succeeded, do the privilege check
    # (skipped in dry-run since we don't actually touch anything).
    if [ "$DRY_RUN" -eq 0 ]; then
        check_root
    else
        info "DRY RUN: skipping root check (no privileged operations will run)."
    fi

    validate_arch
    info "Installing universal binary (covers i386 / x86_64 / arm64 — dyld picks at load time on $(uname -m))"
    info "macOS: $(sw_vers -productVersion 2>/dev/null || echo 'unknown')"

    if [ "$1" != "--local" ]; then
        info "Downloading latest release..."
        if [ "$DRY_RUN" -eq 1 ]; then
            info "DRY RUN: would download ${BINARY_NAME} from GitHub releases."
            BINARY="<dry-run-pending-download>"
        else
            TMPDIR=$(mktemp -d)
            trap "rm -rf $TMPDIR" EXIT

            # v2.5.1+: release asset is `mac-guest-agent` (was
            # `mac-guest-agent-darwin-universal` in v2.5.0 for one day).
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
        echo ""
        info "DRY RUN: would now do:"
        echo "    launchctl stop com.macos.guest-agent"
        echo "    launchctl unload /Library/LaunchDaemons/com.macos.guest-agent.plist"
        echo "    mkdir -p /usr/local/bin"
        echo "    cp \"$BINARY\" \"$INSTALL_PATH\""
        echo "    chmod +x \"$INSTALL_PATH\""
        echo "    \"$INSTALL_PATH\" --install"
        echo "    [serial-device probe]"
        info "DRY RUN complete — no files modified."
        exit 0
    fi

    stop_existing
    info "Installing binary..."
    mkdir -p /usr/local/bin
    cp "$BINARY" "$INSTALL_PATH"
    chmod +x "$INSTALL_PATH"

    info "Installing service..."
    "$INSTALL_PATH" --install

    # Check for serial device
    echo ""
    if check_serial_device; then
        ok "Agent should connect automatically."
    else
        warn "No ISA serial device found."
        echo ""
        echo "  The guest agent requires ISA serial mode on your hypervisor."
        echo "  On Proxmox VE, run this on the host:"
        echo ""
        echo "    qm set <vmid> --agent enabled=1,type=isa"
        echo ""
        echo "  Then restart the VM (stop + start, not reboot)."
        echo "  The agent will connect automatically on next boot."
    fi

    echo ""
    ok "macOS Guest Agent installed."
    echo ""
    echo "  Status:    sudo launchctl list com.macos.guest-agent"
    echo "  Log:       tail -f /var/log/mac-guest-agent.log"
    echo "  Uninstall: sudo $INSTALL_PATH --uninstall"
}

if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "macOS Guest Agent Installer"
    echo ""
    echo "Usage: sudo $0 [--local [PATH]] [--dry-run]"
    echo ""
    echo "  --local         Install from a local binary (for VMs that can't reach GitHub)."
    echo "                  Searches ./mac-guest-agent, /tmp/mac-guest-agent,"
    echo "                  build/mac-guest-agent-universal, build/mac-guest-agent,"
    echo "                  ./mac-guest-agent-darwin-universal,"
    echo "                  /tmp/mac-guest-agent-darwin-universal (pre-v2.5.1 fallback)."
    echo "  --local PATH    Install from an explicit binary path (skips the search)."
    echo "  --dry-run       Print every action the installer would take WITHOUT touching"
    echo "                  /usr/local/bin, /Library/LaunchDaemons, or launchctl. Argument"
    echo "                  validation (--local PATH existence) still happens. Does not"
    echo "                  require root. Combinable with --local."
    echo "  (default)       Download latest release from GitHub"
    echo ""
    echo "Prerequisites:"
    echo "  On the Proxmox VE host, set ISA serial mode:"
    echo "    qm set <vmid> --agent enabled=1,type=isa"
    echo "  Then stop and start the VM."
    exit 0
fi

main "$@"
