#!/bin/bash
# macOS Guest Agent - Installation Script
# Supports Mac OS X 10.4 Tiger through current macOS

set -e

REPO="mav2287/mac-guest-agent"
BINARY_NAME="mac-guest-agent"
INSTALL_PATH="/usr/local/bin/${BINARY_NAME}"

info()  { echo "[INFO] $1" >&2; }
ok()    { echo "[OK]   $1" >&2; }
err()   { echo "[ERR]  $1" >&2; }

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        err "Root privileges required. Run with sudo."
        exit 1
    fi
}

detect_arch() {
    case "$(uname -m)" in
        x86_64)  echo "amd64" ;;
        i386)    echo "amd64" ;;  # 32-bit Intel can run x86_64 on 10.6+
        arm64)   echo "arm64" ;;
        *) err "Unsupported architecture: $(uname -m)"; exit 1 ;;
    esac
}

stop_existing() {
    launchctl stop com.macos.guest-agent 2>/dev/null || true
    launchctl unload /Library/LaunchDaemons/com.macos.guest-agent.plist 2>/dev/null || true
}

main() {
    echo "=== macOS Guest Agent Installer ==="
    check_root

    ARCH=$(detect_arch)
    info "Architecture: $ARCH"
    info "macOS: $(sw_vers -productVersion 2>/dev/null || echo 'unknown')"

    if [ "$1" = "--local" ]; then
        # Local install from build directory
        if [ -f "build/${BINARY_NAME}" ]; then
            BINARY="build/${BINARY_NAME}"
        elif [ -f "./${BINARY_NAME}" ]; then
            BINARY="./${BINARY_NAME}"
        else
            err "No local binary found. Run 'make build' first."
            exit 1
        fi
        info "Using local binary: $BINARY"
    else
        # Download from GitHub
        info "Downloading latest release..."
        TMPDIR=$(mktemp -d)
        trap "rm -rf $TMPDIR" EXIT

        BINARY_FILE="${BINARY_NAME}-darwin-${ARCH}"
        URL="https://github.com/${REPO}/releases/latest/download/${BINARY_FILE}"

        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$TMPDIR/$BINARY_FILE" "$URL" || { err "Download failed"; exit 1; }
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$TMPDIR/$BINARY_FILE" "$URL" || { err "Download failed"; exit 1; }
        else
            err "curl or wget required"
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

    ok "macOS Guest Agent installed and running"
    echo ""
    echo "Commands:"
    echo "  Status:    sudo launchctl list com.macos.guest-agent"
    echo "  Log:       tail -f /var/log/mac-guest-agent.log"
    echo "  Uninstall: sudo $INSTALL_PATH --uninstall"
}

if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Usage: sudo $0 [--local]"
    echo "  --local   Install from local build directory"
    echo "  (default) Download latest release from GitHub"
    exit 0
fi

main "$@"
