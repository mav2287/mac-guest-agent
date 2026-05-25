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

# v2.4.4+: universal-only release. No per-arch detection for asset selection
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
    check_root

    validate_arch
    info "Installing universal binary (covers i386 / x86_64 / arm64 — dyld picks at load time on $(uname -m))"
    info "macOS: $(sw_vers -productVersion 2>/dev/null || echo 'unknown')"

    if [ "$1" = "--local" ]; then
        if [ -f "build/${BINARY_NAME}" ]; then
            BINARY="build/${BINARY_NAME}"
        elif [ -f "./${BINARY_NAME}" ]; then
            BINARY="./${BINARY_NAME}"
        elif [ -f "/tmp/${BINARY_NAME}-x86_64" ]; then
            BINARY="/tmp/${BINARY_NAME}-x86_64"
        elif [ -f "/tmp/${BINARY_NAME}" ]; then
            BINARY="/tmp/${BINARY_NAME}"
        else
            err "No local binary found."
            exit 1
        fi
        info "Using local binary: $BINARY"
    else
        info "Downloading latest release..."
        TMPDIR=$(mktemp -d)
        trap "rm -rf $TMPDIR" EXIT

        BINARY_FILE="${BINARY_NAME}-darwin-universal"
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
    echo "Usage: sudo $0 [--local]"
    echo ""
    echo "  --local   Install from a local binary (for VMs that can't reach GitHub)"
    echo "  (default) Download latest release from GitHub"
    echo ""
    echo "Prerequisites:"
    echo "  On the Proxmox VE host, set ISA serial mode:"
    echo "    qm set <vmid> --agent enabled=1,type=isa"
    echo "  Then stop and start the VM."
    exit 0
fi

main "$@"
