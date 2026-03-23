#!/bin/bash
# Verify a macOS installer image for guest agent compatibility
#
# Usage: ./scripts/verify-installer.sh /path/to/Install*.app
#        ./scripts/verify-installer.sh /path/to/InstallESD.dmg
#        ./scripts/verify-installer.sh /Volumes/MountedInstaller
#
# Checks:
#   - macOS version and build
#   - Required tools (sw_vers, diskutil, sysctl, etc.)
#   - Apple16X50Serial.kext (ISA serial driver)
#   - LaunchDaemons plist DTD support
#   - Default filesystem type (HFS+ vs APFS)
#   - Architecture support
#
# Results are printed in a format suitable for updating docs/COMPATIBILITY.md

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors (if terminal supports them)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RED='\033[0;31m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    GREEN='' YELLOW='' RED='' BOLD='' NC=''
fi

pass() { echo -e "  ${GREEN}PASS${NC}  $1"; }
warn() { echo -e "  ${YELLOW}WARN${NC}  $1"; }
fail() { echo -e "  ${RED}FAIL${NC}  $1"; }
info() { echo -e "  ${BOLD}INFO${NC}  $1"; }

usage() {
    echo "Usage: $0 <installer-path>"
    echo ""
    echo "  installer-path: Path to one of:"
    echo "    - Install macOS *.app bundle"
    echo "    - InstallESD.dmg or BaseSystem.dmg"
    echo "    - Already-mounted installer volume"
    echo ""
    echo "Results can be used to update docs/COMPATIBILITY.md"
    exit 1
}

[ -z "$1" ] && usage

INPUT="$1"
MOUNTED_BY_US=""
SYSTEM_VOL=""

cleanup() {
    if [ -n "$MOUNTED_BY_US" ]; then
        echo ""
        echo "Cleaning up: detaching $MOUNTED_BY_US"
        hdiutil detach "$MOUNTED_BY_US" -quiet 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Resolve the input to a mountable/inspectable system volume
resolve_input() {
    if [ -d "$INPUT/Contents/SharedSupport" ]; then
        # It's an Install macOS *.app bundle
        DMG=""
        if [ -f "$INPUT/Contents/SharedSupport/SharedSupport.dmg" ]; then
            DMG="$INPUT/Contents/SharedSupport/SharedSupport.dmg"
        elif [ -f "$INPUT/Contents/SharedSupport/InstallESD.dmg" ]; then
            DMG="$INPUT/Contents/SharedSupport/InstallESD.dmg"
        elif [ -f "$INPUT/Contents/SharedSupport/BaseSystem.dmg" ]; then
            DMG="$INPUT/Contents/SharedSupport/BaseSystem.dmg"
        fi

        if [ -n "$DMG" ]; then
            echo "Mounting $DMG..."
            MOUNT_OUT=$(hdiutil attach "$DMG" -nobrowse -readonly 2>&1) || {
                echo "Failed to mount $DMG"
                echo "$MOUNT_OUT"
                exit 1
            }
            SYSTEM_VOL=$(echo "$MOUNT_OUT" | grep "/Volumes/" | head -1 | awk -F'\t' '{print $NF}' | sed 's/^ *//')
            MOUNTED_BY_US="$SYSTEM_VOL"
        else
            echo "Could not find a mountable DMG in $INPUT/Contents/SharedSupport/"
            # Try using the app's system volume info directly
            if [ -f "$INPUT/Contents/Info.plist" ]; then
                SYSTEM_VOL="$INPUT"
            else
                exit 1
            fi
        fi
    elif [[ "$INPUT" == *.dmg ]]; then
        echo "Mounting $INPUT..."
        MOUNT_OUT=$(hdiutil attach "$INPUT" -nobrowse -readonly 2>&1) || {
            echo "Failed to mount $INPUT"
            echo "$MOUNT_OUT"
            exit 1
        }
        SYSTEM_VOL=$(echo "$MOUNT_OUT" | grep "/Volumes/" | head -1 | awk -F'\t' '{print $NF}' | sed 's/^ *//')
        MOUNTED_BY_US="$SYSTEM_VOL"
    elif [ -d "$INPUT" ] && [[ "$INPUT" == /Volumes/* ]]; then
        SYSTEM_VOL="$INPUT"
    else
        echo "Cannot determine input type: $INPUT"
        usage
    fi

    if [ -z "$SYSTEM_VOL" ]; then
        echo "Could not resolve system volume from: $INPUT"
        exit 1
    fi

    echo "System volume: $SYSTEM_VOL"
}

# Detect macOS version from the installer
detect_version() {
    echo ""
    echo "${BOLD}[Version]${NC}"

    VERSION=""
    BUILD=""

    # Try SystemVersion.plist (most reliable)
    for plist_path in \
        "$SYSTEM_VOL/System/Library/CoreServices/SystemVersion.plist" \
        "$SYSTEM_VOL/System Library/CoreServices/SystemVersion.plist"; do
        if [ -f "$plist_path" ]; then
            VERSION=$(/usr/libexec/PlistBuddy -c "Print :ProductVersion" "$plist_path" 2>/dev/null || true)
            BUILD=$(/usr/libexec/PlistBuddy -c "Print :ProductBuildVersion" "$plist_path" 2>/dev/null || true)
            break
        fi
    done

    # Try the app bundle's Info.plist
    if [ -z "$VERSION" ] && [ -f "$INPUT/Contents/Info.plist" ]; then
        VERSION=$(/usr/libexec/PlistBuddy -c "Print :DTPlatformVersion" "$INPUT/Contents/Info.plist" 2>/dev/null || true)
    fi

    if [ -n "$VERSION" ]; then
        pass "macOS version: $VERSION (build: ${BUILD:-unknown})"
    else
        fail "Could not detect macOS version"
    fi
}

# Check for required tools in the installer's system
check_tools() {
    echo ""
    echo "${BOLD}[Tools]${NC}"

    TOOLS=(
        "usr/bin/sw_vers:OS version detection:required"
        "usr/sbin/diskutil:disk information:required"
        "usr/sbin/sysctl:hardware info:required"
        "usr/bin/osascript:graceful shutdown:optional"
        "sbin/shutdown:shutdown fallback:required"
        "usr/bin/pmset:suspend/hibernate:optional"
        "usr/bin/dscl:password changes:optional"
        "bin/launchctl:service management:required"
        "usr/bin/tmutil:APFS snapshots:optional"
        "usr/sbin/netstat:network info fallback:optional"
    )

    for entry in "${TOOLS[@]}"; do
        IFS=':' read -r path purpose level <<< "$entry"
        # Check both with and without leading /
        if [ -f "$SYSTEM_VOL/$path" ] || [ -f "$SYSTEM_VOL/usr/bin/$(basename "$path")" ]; then
            pass "$(basename "$path") ($purpose)"
        else
            if [ "$level" = "required" ]; then
                fail "$(basename "$path") not found ($purpose)"
            else
                warn "$(basename "$path") not found ($purpose)"
            fi
        fi
    done
}

# Check for Apple16X50Serial.kext (ISA serial driver)
check_serial_kext() {
    echo ""
    echo "${BOLD}[ISA Serial Driver]${NC}"

    KEXT_PATHS=(
        "$SYSTEM_VOL/System/Library/Extensions/Apple16X50Serial.kext"
        "$SYSTEM_VOL/System/Library/Extensions/IOSerialFamily.kext"
    )

    found_serial=0
    for kext in "${KEXT_PATHS[@]}"; do
        if [ -d "$kext" ]; then
            kext_name=$(basename "$kext")
            pass "$kext_name present"
            found_serial=1

            # Check Info.plist for driver details
            kext_plist="$kext/Contents/Info.plist"
            if [ -f "$kext_plist" ]; then
                bundle_id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$kext_plist" 2>/dev/null || true)
                if [ -n "$bundle_id" ]; then
                    info "bundle: $bundle_id"
                fi
            fi
        fi
    done

    if [ "$found_serial" -eq 0 ]; then
        fail "No serial driver kext found (Apple16X50Serial.kext or IOSerialFamily.kext)"
    fi
}

# Check LaunchDaemon plist compatibility
check_launchdaemon() {
    echo ""
    echo "${BOLD}[LaunchDaemon Support]${NC}"

    LAUNCHD_DIR="$SYSTEM_VOL/System/Library/LaunchDaemons"
    if [ -d "$LAUNCHD_DIR" ]; then
        pass "LaunchDaemons directory exists"

        # Check if any existing plist uses KeepAlive (our format)
        if ls "$LAUNCHD_DIR"/*.plist >/dev/null 2>&1; then
            sample=$(ls "$LAUNCHD_DIR"/*.plist | head -1)
            if grep -q "KeepAlive" "$sample" 2>/dev/null; then
                pass "KeepAlive key supported (found in system plists)"
            else
                info "KeepAlive key not found in sample plist (may still be supported)"
            fi
        fi
    else
        warn "LaunchDaemons directory not found"
    fi
}

# Detect default filesystem type
check_filesystem() {
    echo ""
    echo "${BOLD}[Filesystem]${NC}"

    # Check if this is APFS or HFS+
    if [ -n "$VERSION" ]; then
        major=$(echo "$VERSION" | cut -d. -f1)
        minor=$(echo "$VERSION" | cut -d. -f2)

        if [ "$major" -gt 10 ] || ([ "$major" -eq 10 ] && [ "$minor" -ge 13 ]); then
            info "Default filesystem: APFS (10.13+)"
            info "Freeze: sync + F_FULLFSYNC + tmutil snapshot"
        else
            info "Default filesystem: HFS+"
            info "Freeze: sync + F_FULLFSYNC only (no APFS snapshots)"
        fi
    fi

    # Check for diskutil in the installer to see APFS support
    if [ -f "$SYSTEM_VOL/usr/sbin/diskutil" ]; then
        # Check if diskutil mentions apfs
        if strings "$SYSTEM_VOL/usr/sbin/diskutil" 2>/dev/null | grep -qi "apfs"; then
            pass "diskutil has APFS support"
        else
            info "diskutil does not reference APFS (pre-10.13)"
        fi
    fi
}

# Detect architecture support
check_architecture() {
    echo ""
    echo "${BOLD}[Architecture]${NC}"

    # Check a known system binary for architecture
    for binary in "$SYSTEM_VOL/usr/bin/sw_vers" "$SYSTEM_VOL/bin/ls" "$SYSTEM_VOL/usr/bin/true"; do
        if [ -f "$binary" ]; then
            archs=$(file "$binary" 2>/dev/null | grep -oE '(x86_64|i386|arm64|arm64e)' | sort -u | tr '\n' ' ')
            if [ -n "$archs" ]; then
                pass "Supported architectures: $archs"
            fi
            break
        fi
    done

    if [ -n "$VERSION" ]; then
        major=$(echo "$VERSION" | cut -d. -f1)
        minor=$(echo "$VERSION" | cut -d. -f2)

        if [ "$major" -gt 10 ]; then
            info "Agent binary: arm64 (mac-guest-agent-darwin-arm64)"
        elif [ "$major" -eq 10 ] && [ "$minor" -ge 6 ]; then
            info "Agent binary: x86_64 (mac-guest-agent-darwin-amd64)"
        elif [ "$major" -eq 10 ] && [ "$minor" -lt 6 ]; then
            info "Agent binary: i386 (mac-guest-agent-i386, if available)"
        fi
    fi
}

# Print summary for COMPATIBILITY.md
print_summary() {
    echo ""
    echo "========================================="
    echo "${BOLD}Summary for docs/COMPATIBILITY.md${NC}"
    echo "========================================="
    echo ""

    NAME=""
    if [ -n "$VERSION" ]; then
        major=$(echo "$VERSION" | cut -d. -f1)
        minor=$(echo "$VERSION" | cut -d. -f2)

        # Map version to name
        if [ "$major" -eq 10 ]; then
            case "$minor" in
                4) NAME="Tiger" ;;
                5) NAME="Leopard" ;;
                6) NAME="Snow Leopard" ;;
                7) NAME="Lion" ;;
                8) NAME="Mountain Lion" ;;
                9) NAME="Mavericks" ;;
                10) NAME="Yosemite" ;;
                11) NAME="El Capitan" ;;
                12) NAME="Sierra" ;;
                13) NAME="High Sierra" ;;
                14) NAME="Mojave" ;;
                15) NAME="Catalina" ;;
            esac
        else
            case "$major" in
                11) NAME="Big Sur" ;;
                12) NAME="Monterey" ;;
                13) NAME="Ventura" ;;
                14) NAME="Sonoma" ;;
                15) NAME="Sequoia" ;;
                26) NAME="Tahoe" ;;
            esac
        fi

        echo "| $VERSION ${NAME:+$NAME} | 2 | Binary builds | Untested | Untested | Untested | Untested | Untested | Installer verified ($(date +%Y-%m-%d)) |"
    fi

    echo ""
    echo "To upgrade this to Tier 1, run the agent in a VM:"
    echo "  1. Install the binary"
    echo "  2. Run: sudo mac-guest-agent --self-test"
    echo "  3. Run: ./tests/safe_test.sh /usr/local/bin/mac-guest-agent"
    echo "  4. Test PVE integration: qm agent <vmid> ping"
    echo "  5. Test freeze: qm guest cmd <vmid> fsfreeze-freeze"
}

# Main
echo "macOS Installer Verification for Guest Agent Compatibility"
echo "==========================================================="

resolve_input
detect_version
check_tools
check_serial_kext
check_launchdaemon
check_filesystem
check_architecture
print_summary
