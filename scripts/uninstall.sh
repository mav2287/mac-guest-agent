#!/bin/bash
# macOS Guest Agent - Uninstall Script

set -e

BINARY_PATH="/usr/local/bin/mac-guest-agent"

if [ "$(id -u)" -ne 0 ]; then
    echo "[ERR] Root privileges required. Run with sudo." >&2
    exit 1
fi

echo "=== macOS Guest Agent Uninstaller ==="

if [ -f "$BINARY_PATH" ]; then
    "$BINARY_PATH" --uninstall
else
    # Manual cleanup if binary is already gone
    launchctl stop com.macos.guest-agent 2>/dev/null || true
    launchctl unload /Library/LaunchDaemons/com.macos.guest-agent.plist 2>/dev/null || true
    rm -f /Library/LaunchDaemons/com.macos.guest-agent.plist
    rm -rf /usr/local/share/mac-guest-agent
    echo "Cleaned up manually (binary was already removed)"
fi

echo "Done."
