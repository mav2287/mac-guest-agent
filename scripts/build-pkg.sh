#!/bin/bash
# Build a macOS .pkg installer for the guest agent
# Usage: ./scripts/build-pkg.sh [arch]
#   arch: universal (default), amd64, arm64, or i386 (single-slice builds for testing only)
#
# Produces: build/mac-guest-agent-<version>-<arch>.pkg
#
# The .pkg can be installed by:
#   - Double-clicking in Finder
#   - sudo installer -pkg mac-guest-agent-*.pkg -target /
#
# Prereq: the named slice must already exist under build/. The documented
# release flow is `make pkg`, which depends on `make build-all` and produces
# the universal slice. `make build-all` requires the legacy 10.13 SDK
# (see README "Building from Source" for LEGACY_SDK setup). Single-slice
# invocations for i386 and x86_64 require the same SDK; only `arm64` builds
# with the host Xcode SDK alone.

set -e

# Single-source the version from the Makefile so a release tag never
# ships a .pkg whose stamped version disagrees with the binary. Honour
# a VERSION env override (used by .github/workflows/release.yml to stamp
# the git-tag version on tagged releases). See audit.md finding 4.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="${VERSION:-$(awk -F':= *' '/^VERSION[[:space:]]*:=/{print $2; exit}' "$SCRIPT_DIR/../Makefile")}"
if [ -z "$VERSION" ]; then
    echo "Error: could not determine VERSION (Makefile parse failed and no env override)" >&2
    exit 1
fi
ARCH="${1:-universal}"
PKG_ID="com.github.mac-guest-agent"
PKG_NAME="mac-guest-agent-${VERSION}-${ARCH}.pkg"

echo "=== Building .pkg installer ==="
echo "Version: $VERSION"
echo "Architecture: $ARCH"

# Determine binary name
case "$ARCH" in
    universal) BINARY="build/mac-guest-agent-universal" ;;
    amd64)  BINARY="build/mac-guest-agent-x86_64" ;;
    arm64)  BINARY="build/mac-guest-agent-arm64" ;;
    i386)   BINARY="build/mac-guest-agent-i386" ;;
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
