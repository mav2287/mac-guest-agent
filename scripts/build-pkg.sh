#!/bin/bash
# Build a macOS .pkg installer for the guest agent
# Usage: ./scripts/build-pkg.sh [arch]
#   arch: amd64, arm64, or universal (default: current arch)
#
# Produces: build/mac-guest-agent-<version>-<arch>.pkg
#
# The .pkg can be installed by:
#   - Double-clicking in Finder
#   - sudo installer -pkg mac-guest-agent-*.pkg -target /

set -e

VERSION="2.3.1"
ARCH="${1:-$(uname -m | sed 's/x86_64/amd64/;s/arm64/arm64/')}"
PKG_ID="com.github.mac-guest-agent"
PKG_NAME="mac-guest-agent-${VERSION}-${ARCH}.pkg"

echo "=== Building .pkg installer ==="
echo "Version: $VERSION"
echo "Architecture: $ARCH"

# Determine binary name
case "$ARCH" in
    amd64)  BINARY="build/mac-guest-agent-x86_64" ;;
    arm64)  BINARY="build/mac-guest-agent-arm64" ;;
    universal) BINARY="build/mac-guest-agent-universal" ;;
    *) echo "Unknown arch: $ARCH"; exit 1 ;;
esac

if [ ! -f "$BINARY" ]; then
    echo "Binary not found: $BINARY"
    echo "Run 'make build-all' first."
    exit 1
fi

# Create staging directory
STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

# Stage files
mkdir -p "$STAGE/root/usr/local/bin"
mkdir -p "$STAGE/root/usr/local/share/man/man8"
mkdir -p "$STAGE/root/etc/qemu"
mkdir -p "$STAGE/scripts"

cp "$BINARY" "$STAGE/root/usr/local/bin/mac-guest-agent"
chmod 755 "$STAGE/root/usr/local/bin/mac-guest-agent"

cp docs/mac-guest-agent.8 "$STAGE/root/usr/local/share/man/man8/"
cp configs/qemu-ga.conf "$STAGE/root/etc/qemu/qemu-ga.conf.default"

# Post-install script: register the LaunchDaemon and start the service
cat > "$STAGE/scripts/postinstall" << 'POSTEOF'
#!/bin/bash
# Post-install: register LaunchDaemon and start service
/usr/local/bin/mac-guest-agent --install 2>/dev/null || true
echo "macOS Guest Agent installed."
echo ""
echo "IMPORTANT: Set ISA serial mode on the Proxmox VE host:"
echo "  qm set <vmid> --agent enabled=1,type=isa"
echo "Then stop and start the VM."
exit 0
POSTEOF
chmod 755 "$STAGE/scripts/postinstall"

# Pre-install script: stop existing service
cat > "$STAGE/scripts/preinstall" << 'PREEOF'
#!/bin/bash
# Pre-install: stop existing service if running
launchctl stop com.macos.guest-agent 2>/dev/null || true
launchctl unload /Library/LaunchDaemons/com.macos.guest-agent.plist 2>/dev/null || true
exit 0
PREEOF
chmod 755 "$STAGE/scripts/preinstall"

# Build component package
echo "Building component package..."
pkgbuild \
    --root "$STAGE/root" \
    --scripts "$STAGE/scripts" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location "/" \
    "build/$PKG_NAME"

echo ""
echo "=== Package built: build/$PKG_NAME ==="
echo ""
echo "Install via terminal:"
echo "  sudo installer -pkg build/$PKG_NAME -target /"
echo ""
echo "Install via UI:"
echo "  Double-click build/$PKG_NAME in Finder"
ls -lh "build/$PKG_NAME"
