#!/bin/bash
# Build a macOS .pkg installer for the guest agent
# Usage: ./scripts/build-pkg.sh [arch]
#   arch: universal (default), amd64, arm64, or i386 (single-slice builds for testing only)
#
# Produces: build/mac-guest-agent-<version>-<arch>.pkg
#
# Install (terminal — supported for both unsigned and signed packages):
#   sudo installer -pkg build/mac-guest-agent-<version>-universal.pkg -target /
#
# Install (Finder double-click): only viable for SIGNED packages. Modern
# macOS Gatekeeper rejects unsigned packages from Finder with a
# "[pkg name] can't be opened because it is from an unidentified developer"
# dialog. If you want a double-click-installable pkg, set the env var
# PRODUCTSIGN_IDENTITY to a Developer ID Installer identity:
#   PRODUCTSIGN_IDENTITY="Developer ID Installer: Your Name (TEAMID)" \
#       ./scripts/build-pkg.sh universal
# The script will sign the produced pkg with productsign after pkgbuild.
# Notarization is a separate step (xcrun notarytool submit / staple) not
# automated here.
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
# the git-tag version on tagged releases).
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

# Note on `._*` entries in the BOM: pkgutil --payload-files and lsbom
# will list one `._*` sibling per real file AND per directory (e.g.
# `./usr/local/bin/._mac-guest-agent`, `./usr/local/._bin`). These are
# NOT real files in the payload — `pkgutil --expand-full` shows zero
# `._*` files in Payload/. They're pkgbuild's encoding of per-entry
# extended-attribute / ACL metadata in the xar BOM. Suppressing them
# would require bypassing pkgbuild entirely (custom xar packing) which
# isn't worth the cost. They have no install-time side effect.
# Defensive cleanup of any actual `._*` files that `cp` from HFS+
# sources might have left behind in the staging tree:
find "$STAGE/root" -name '._*' -delete 2>/dev/null || true

# Post-install script: register the LaunchDaemon and start the service.
#
# Audit 2026-05-29 finding MED-2: previously this script suppressed
# --install errors (`2>/dev/null || true`) and always exited 0, so a
# package installation could report success even if:
#   - the binary couldn't execute on the host architecture
#   - --install failed to write the LaunchDaemon plist
#   - launchctl load/start failed
# That hid real installation failures behind a "macOS Guest Agent installed."
# message. v2.5.3+: --install errors surface to stderr, postinstall exits
# non-zero on failure so Installer.app (or `installer` CLI) reports the
# package as failed. uname -m gate added so unsupported architectures fail
# with the same clear message scripts/install.sh produces.
cat > "$STAGE/scripts/postinstall" << 'POSTEOF'
#!/bin/bash
# Post-install: register LaunchDaemon and start service.
set -e

# Architecture gate (matches scripts/install.sh's validate_arch).
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|i386|i486|i586|i686|arm64|arm64e)
        ;;
    *)
        echo "Error: unsupported architecture: $ARCH" >&2
        echo "This package ships i386, x86_64, and arm64 slices. PowerPC and other architectures are not supported." >&2
        exit 1
        ;;
esac

# Run --install. Errors surface; non-zero exit fails the package install.
if ! /usr/local/bin/mac-guest-agent --install; then
    echo "Error: mac-guest-agent --install failed." >&2
    echo "       Check the output above. Common causes:" >&2
    echo "         - LaunchDaemon plist could not be written (filesystem permissions)" >&2
    echo "         - launchctl load/start refused the daemon (check /var/log/system.log)" >&2
    exit 1
fi

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

# Optional signing for Finder-double-click distribution.
if [ -n "${PRODUCTSIGN_IDENTITY:-}" ]; then
    SIGNED_PKG="build/${PKG_NAME%.pkg}-signed.pkg"
    echo "Signing with: $PRODUCTSIGN_IDENTITY"
    if productsign --sign "$PRODUCTSIGN_IDENTITY" "build/$PKG_NAME" "$SIGNED_PKG"; then
        echo "  Signed package: $SIGNED_PKG"
        echo "  (Notarization is a separate step — see xcrun notarytool / stapler.)"
        echo ""
    else
        echo "  productsign FAILED — unsigned package at build/$PKG_NAME is still usable via terminal." >&2
    fi
fi

echo "Install via terminal (works for both signed and unsigned packages):"
echo "  sudo installer -pkg build/$PKG_NAME -target /"

if pkgutil --check-signature "build/$PKG_NAME" 2>/dev/null | grep -q 'Status: signed'; then
    echo ""
    echo "Install via UI (signed package — Finder double-click):"
    echo "  open build/$PKG_NAME"
else
    echo ""
    echo "NOTE: this package is UNSIGNED. Gatekeeper will reject a Finder"
    echo "double-click on modern macOS. Use the terminal install above, or"
    echo "rebuild with PRODUCTSIGN_IDENTITY set to a Developer ID Installer"
    echo "identity (see the script header for details)."
fi
ls -lh "build/$PKG_NAME"
