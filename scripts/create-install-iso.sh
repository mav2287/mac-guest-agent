#!/bin/bash
# Create a bootable macOS install ISO from an installer .app
#
# Usage: ./scripts/create-install-iso.sh /path/to/Install*.app [output.iso]
#
# Works with macOS 10.9 Mavericks through current.
# For older versions (10.7-10.8), the InstallESD.dmg can be used directly.
#
# The resulting ISO can be used with QEMU/Proxmox VE to create macOS VMs.

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <installer.app> [output.iso]"
    echo ""
    echo "Creates a bootable ISO from a macOS installer .app bundle."
    echo "The ISO can be attached to a QEMU/PVE VM for installation."
    exit 1
fi

INSTALLER="$1"
APP_NAME=$(basename "$INSTALLER" .app)

if [ ! -d "$INSTALLER/Contents/SharedSupport" ]; then
    echo "Error: Not a valid macOS installer app bundle."
    echo "Expected: $INSTALLER/Contents/SharedSupport/"
    exit 1
fi

# Determine output path
if [ -n "$2" ]; then
    OUTPUT="$2"
else
    OUTPUT="$(dirname "$INSTALLER")/${APP_NAME}.iso"
fi

# Detect installer type
HAS_CREATEINSTALLMEDIA=0
if [ -f "$INSTALLER/Contents/Resources/createinstallmedia" ]; then
    HAS_CREATEINSTALLMEDIA=1
fi

# For modern installers (10.9+), use createinstallmedia
if [ "$HAS_CREATEINSTALLMEDIA" -eq 1 ]; then
    echo "Creating bootable ISO from: $APP_NAME"
    echo "Output: $OUTPUT"
    echo ""

    # Create a temporary sparse disk image
    TMPDIR=$(mktemp -d)
    trap "rm -rf $TMPDIR" EXIT

    DMG="$TMPDIR/install_media.dmg"
    SPARSEIMAGE="$TMPDIR/install_media.sparseimage"
    VOLUME_NAME="Install"

    # Determine size needed (installer size + 1GB headroom)
    INSTALLER_SIZE_MB=$(du -sm "$INSTALLER" 2>/dev/null | awk '{print $1}')
    IMAGE_SIZE_MB=$((INSTALLER_SIZE_MB + 1024))
    echo "Installer size: ${INSTALLER_SIZE_MB}MB, image size: ${IMAGE_SIZE_MB}MB"

    # Create sparse image
    echo "Creating disk image..."
    hdiutil create -size "${IMAGE_SIZE_MB}m" -fs HFS+J -volname "$VOLUME_NAME" \
        -type SPARSE "$SPARSEIMAGE" -quiet

    # Attach it
    echo "Mounting disk image..."
    MOUNT_OUT=$(hdiutil attach "${SPARSEIMAGE}.sparseimage" -nobrowse -readwrite 2>&1)
    MOUNT_DEV=$(echo "$MOUNT_OUT" | grep "/dev/disk" | tail -1 | awk '{print $1}')
    MOUNT_VOL=$(echo "$MOUNT_OUT" | grep "/Volumes/" | tail -1 | awk -F'\t' '{print $NF}' | sed 's/^ *//')

    if [ -z "$MOUNT_VOL" ]; then
        echo "Error: Failed to mount disk image"
        echo "$MOUNT_OUT"
        exit 1
    fi

    echo "Running createinstallmedia (this may take a while)..."
    sudo "$INSTALLER/Contents/Resources/createinstallmedia" \
        --volume "$MOUNT_VOL" \
        --nointeraction 2>&1 | tail -5

    # Find the new volume name (createinstallmedia renames it)
    NEW_VOL=$(ls -d /Volumes/Install* 2>/dev/null | head -1)
    if [ -z "$NEW_VOL" ]; then
        echo "Error: createinstallmedia did not produce a volume"
        exit 1
    fi

    echo "Detaching..."
    hdiutil detach "$MOUNT_DEV" -quiet 2>/dev/null || hdiutil detach "$NEW_VOL" -quiet 2>/dev/null

    # Convert sparse image to ISO (CDR format, then rename)
    echo "Converting to ISO..."
    hdiutil convert "${SPARSEIMAGE}.sparseimage" -format UDTO \
        -o "$TMPDIR/install_media" -quiet
    mv "$TMPDIR/install_media.cdr" "$OUTPUT"

    echo ""
    echo "ISO created: $OUTPUT"
    ls -lh "$OUTPUT"
    echo ""
    echo "To use with Proxmox VE:"
    echo "  1. Upload to PVE storage (e.g., /var/lib/vz/template/iso/)"
    echo "  2. Attach as CD/DVD drive when creating the VM"
    echo "  3. Boot from the ISO with OpenCore"

else
    # For older installers (10.7-10.8), just convert the InstallESD.dmg
    ESD="$INSTALLER/Contents/SharedSupport/InstallESD.dmg"
    if [ ! -f "$ESD" ]; then
        echo "Error: No InstallESD.dmg or createinstallmedia found"
        exit 1
    fi

    echo "Converting InstallESD.dmg to ISO..."
    hdiutil convert "$ESD" -format UDTO -o "${OUTPUT%.iso}" -quiet
    if [ -f "${OUTPUT%.iso}.cdr" ]; then
        mv "${OUTPUT%.iso}.cdr" "$OUTPUT"
    fi

    echo ""
    echo "ISO created: $OUTPUT"
    ls -lh "$OUTPUT"
fi
