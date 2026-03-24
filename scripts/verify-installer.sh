#!/bin/bash
# Verify a macOS installer image for guest agent compatibility
#
# Usage: ./scripts/verify-installer.sh /path/to/Install*.app
#        ./scripts/verify-installer.sh /path/to/InstallESD.dmg
#        ./scripts/verify-installer.sh /Volumes/MountedInstaller
#
# Performs deep verification:
#   - macOS version and build
#   - Required tools (sw_vers, diskutil, sysctl, etc.)
#   - Apple16X50Serial.kext presence and PCI class matching
#   - Critical C library symbols (getifaddrs, getutxent, poll, etc.)
#   - CoreFoundation and IOKit frameworks
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

PASSES=0
WARNS=0
FAILS=0
JSON_CHECKS=""

json_add() {
    local level="$1" name="$2"
    # Escape quotes for JSON
    name=$(echo "$name" | sed 's/"/\\"/g')
    local entry="{\"level\":\"$level\",\"name\":\"$name\"}"
    if [ -n "$JSON_CHECKS" ]; then
        JSON_CHECKS="$JSON_CHECKS,$entry"
    else
        JSON_CHECKS="$entry"
    fi
}

pass() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "  ${GREEN}PASS${NC}  $1"; fi
    json_add "pass" "$1"
    PASSES=$((PASSES + 1))
}
warn() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "  ${YELLOW}WARN${NC}  $1"; fi
    json_add "warn" "$1"
    WARNS=$((WARNS + 1))
}
fail() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "  ${RED}FAIL${NC}  $1"; fi
    json_add "fail" "$1"
    FAILS=$((FAILS + 1))
}
info() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "  ${BOLD}INFO${NC}  $1"; fi
    json_add "info" "$1"
}

JSON_OUTPUT=0

usage() {
    echo "Usage: $0 [--json] <installer-path>"
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo "  --json            Output machine-readable JSON"
    echo "  installer-path:   Path to one of:"
    echo "    - Install macOS *.app bundle"
    echo "    - InstallESD.dmg or BaseSystem.dmg"
    echo "    - Already-mounted installer volume"
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo "Results can be used to update docs/COMPATIBILITY.md"
    exit 1
}

if [ "$1" = "--json" ]; then
    JSON_OUTPUT=1
    shift
fi

[ -z "$1" ] && usage

INPUT="$1"
MOUNTS_TO_DETACH=()
SYSTEM_VOL=""

cleanup() {
    for ((i=${#MOUNTS_TO_DETACH[@]}-1; i>=0; i--)); do
        if [ "$JSON_OUTPUT" -eq 0 ]; then echo "Cleaning up: detaching ${MOUNTS_TO_DETACH[$i]}"; fi
        hdiutil detach "${MOUNTS_TO_DETACH[$i]}" -quiet 2>/dev/null || true
    done
}
trap cleanup EXIT

# Mount a DMG and return the volume path
mount_dmg() {
    local dmg="$1"
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo "Mounting $dmg..." >&2; fi
    local mount_out
    mount_out=$(hdiutil attach "$dmg" -nobrowse -readonly 2>&1) || {
        echo "Failed to mount $dmg" >&2
        echo "$mount_out" >&2
        return 1
    }
    local vol
    vol=$(echo "$mount_out" | grep "/Volumes/" | head -1 | awk -F'\t' '{print $NF}' | sed 's/^ *//')
    if [ -n "$vol" ]; then
        MOUNTS_TO_DETACH+=("$vol")
        echo "$vol"
    else
        return 1
    fi
}

# Resolve the input to the actual system volume with tools and kexts
resolve_input() {
    local esd_vol=""

    if [ -d "$INPUT/Contents/SharedSupport" ]; then
        if [ -f "$INPUT/Contents/SharedSupport/SharedSupport.dmg" ]; then
            esd_vol=$(mount_dmg "$INPUT/Contents/SharedSupport/SharedSupport.dmg") || true
        fi
        if [ -z "$esd_vol" ] && [ -f "$INPUT/Contents/SharedSupport/InstallESD.dmg" ]; then
            esd_vol=$(mount_dmg "$INPUT/Contents/SharedSupport/InstallESD.dmg") || true
        fi
        if [ -z "$esd_vol" ] && [ -f "$INPUT/Contents/SharedSupport/BaseSystem.dmg" ]; then
            SYSTEM_VOL=$(mount_dmg "$INPUT/Contents/SharedSupport/BaseSystem.dmg") || true
        fi

        if [ -n "$esd_vol" ] && [ -z "$SYSTEM_VOL" ]; then
            # Check for BaseSystem.dmg inside the ESD (10.7-10.12 style)
            if [ -f "$esd_vol/BaseSystem.dmg" ]; then
                SYSTEM_VOL=$(mount_dmg "$esd_vol/BaseSystem.dmg") || true
            fi
            # If ESD has no BaseSystem inside, try sibling BaseSystem.dmg (10.14+ style)
            if [ -z "$SYSTEM_VOL" ] && [ -f "$INPUT/Contents/SharedSupport/BaseSystem.dmg" ]; then
                SYSTEM_VOL=$(mount_dmg "$INPUT/Contents/SharedSupport/BaseSystem.dmg") || true
            fi
            if [ -z "$SYSTEM_VOL" ]; then
                SYSTEM_VOL="$esd_vol"
            fi
        fi

        if [ -z "$SYSTEM_VOL" ]; then
            SYSTEM_VOL="$INPUT"
        fi

    elif [[ "$INPUT" == *.dmg ]]; then
        local vol
        vol=$(mount_dmg "$INPUT") || exit 1
        if [ -f "$vol/BaseSystem.dmg" ]; then
            SYSTEM_VOL=$(mount_dmg "$vol/BaseSystem.dmg") || true
            if [ -z "$SYSTEM_VOL" ]; then
                SYSTEM_VOL="$vol"
            fi
        else
            SYSTEM_VOL="$vol"
        fi

    elif [ -d "$INPUT" ] && [[ "$INPUT" == /Volumes/* ]]; then
        if [ -f "$INPUT/BaseSystem.dmg" ]; then
            SYSTEM_VOL=$(mount_dmg "$INPUT/BaseSystem.dmg") || true
            if [ -z "$SYSTEM_VOL" ]; then
                SYSTEM_VOL="$INPUT"
            fi
        else
            SYSTEM_VOL="$INPUT"
        fi
    else
        echo "Cannot determine input type: $INPUT"
        usage
    fi

    if [ -z "$SYSTEM_VOL" ]; then
        echo "Could not resolve system volume from: $INPUT"
        exit 1
    fi

    if [ "$JSON_OUTPUT" -eq 0 ]; then echo "System volume: $SYSTEM_VOL"; fi
}

# Detect macOS version from the installer
detect_version() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[Version]${NC}"; fi

    VERSION=""
    BUILD=""

    for plist_path in \
        "$SYSTEM_VOL/System/Library/CoreServices/SystemVersion.plist" \
        "$SYSTEM_VOL/System Library/CoreServices/SystemVersion.plist"; do
        if [ -f "$plist_path" ]; then
            VERSION=$(/usr/libexec/PlistBuddy -c "Print :ProductVersion" "$plist_path" 2>/dev/null || true)
            BUILD=$(/usr/libexec/PlistBuddy -c "Print :ProductBuildVersion" "$plist_path" 2>/dev/null || true)
            break
        fi
    done

    if [ -z "$VERSION" ] || [ "$VERSION" = "GM" ]; then
        if [ -f "$INPUT/Contents/Info.plist" ]; then
            local app_ver
            app_ver=$(/usr/libexec/PlistBuddy -c "Print :DTPlatformVersion" "$INPUT/Contents/Info.plist" 2>/dev/null || true)
            if [ -n "$app_ver" ] && [ "$app_ver" != "GM" ]; then
                VERSION="$app_ver"
            fi
        fi
    fi

    if [ -z "$VERSION" ] || [ "$VERSION" = "GM" ]; then
        local basename_input
        basename_input=$(basename "$INPUT")
        local name_ver
        name_ver=$(echo "$basename_input" | grep -oE '10\.[0-9]+|1[1-5]\.[0-9]+' | head -1)
        if [ -n "$name_ver" ]; then
            VERSION="$name_ver"
            info "Version extracted from filename (not from system plist)"
        fi
    fi

    if [ -n "$VERSION" ] && [ "$VERSION" != "GM" ]; then
        pass "macOS version: $VERSION (build: ${BUILD:-unknown})"
    else
        fail "Could not detect macOS version"
    fi
}

# Check for required tools in the installer's system
check_tools() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[Tools]${NC}"; fi

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
        local tool_name
        tool_name=$(basename "$path")
        if [ -f "$SYSTEM_VOL/$path" ]; then
            pass "$tool_name ($purpose)"
        else
            if [ "$level" = "required" ]; then
                fail "$tool_name not found ($purpose)"
            else
                warn "$tool_name not found ($purpose)"
            fi
        fi
    done
}

# Check for Apple16X50Serial.kext and verify PCI class matching
check_serial_kext() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[ISA Serial Driver]${NC}"; fi

    local kext="$SYSTEM_VOL/System/Library/Extensions/Apple16X50Serial.kext"
    local ioserial="$SYSTEM_VOL/System/Library/Extensions/IOSerialFamily.kext"

    found_serial=0

    if [ -d "$kext" ]; then
        pass "Apple16X50Serial.kext present"
        found_serial=1

        local kext_plist="$kext/Contents/Info.plist"
        if [ -f "$kext_plist" ]; then
            local bundle_id class_match
            bundle_id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$kext_plist" 2>/dev/null || true)
            class_match=$(grep -A1 "IOPCIClassMatch" "$kext_plist" 2>/dev/null | grep "<string>" | sed 's/.*<string>\(.*\)<\/string>.*/\1/' || true)

            if [ -n "$bundle_id" ]; then
                info "bundle: $bundle_id"
            fi

            # Verify the kext matches PCI class 0x0700 (Serial Controller)
            # QEMU's ISA serial appears as this PCI class
            # Normalize XML entity encoding
            class_match=$(echo "$class_match" | sed 's/&amp;/\&/g')
            if [ "$class_match" = "0x07000000&0xFFFF0000" ]; then
                pass "PCI class match: 0x0700 (Serial Controller) — matches QEMU ISA serial"
            elif [ -n "$class_match" ]; then
                warn "PCI class match: $class_match (unexpected, may not match QEMU ISA serial)"
            else
                warn "Could not read IOPCIClassMatch from kext plist"
            fi
        fi
    else
        fail "Apple16X50Serial.kext not found"
    fi

    if [ -d "$ioserial" ]; then
        pass "IOSerialFamily.kext present (serial port framework)"
    else
        warn "IOSerialFamily.kext not found"
    fi
}

# Deep check: verify critical C library symbols exist in the installer's system libraries
check_symbols() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[C Library Symbols]${NC}"; fi

    local syslib_dir="$SYSTEM_VOL/usr/lib/system"
    if [ ! -d "$syslib_dir" ]; then
        warn "System library directory not found ($syslib_dir), skipping symbol check"
        return
    fi

    # These are the non-trivial symbols our agent depends on
    # (beyond basic libc like malloc/free/printf which are guaranteed)
    local CRITICAL_SYMBOLS=(
        _getifaddrs _freeifaddrs _getutxent _endutxent _getloadavg
        _getmntinfo _getpwnam _sysctlbyname _gettimeofday _settimeofday
        _host_statistics _host_statistics64 _poll _strtok_r _fcntl
        _sync _tcgetattr _tcsetattr _tcflush _tcdrain
    )

    local missing=0
    local found=0

    # Dump all exported text symbols from system sub-libraries into a temp file
    local symfile
    symfile=$(mktemp)
    nm -g "$syslib_dir"/*.dylib 2>/dev/null | grep " T _" | awk '{print $NF}' | sort -u > "$symfile"

    for sym in "${CRITICAL_SYMBOLS[@]}"; do
        if grep -qx "$sym" "$symfile" 2>/dev/null; then
            found=$((found + 1))
        else
            fail "symbol missing: $sym"
            missing=$((missing + 1))
        fi
    done

    rm -f "$symfile"

    if [ "$missing" -eq 0 ]; then
        pass "all ${#CRITICAL_SYMBOLS[@]} critical symbols present"
    else
        fail "$missing of ${#CRITICAL_SYMBOLS[@]} critical symbols missing"
    fi
}

# Check required frameworks
check_frameworks() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[Frameworks]${NC}"; fi

    local fw_dir="$SYSTEM_VOL/System/Library/Frameworks"

    if [ -d "$fw_dir/CoreFoundation.framework" ]; then
        pass "CoreFoundation.framework present"
    else
        fail "CoreFoundation.framework not found"
    fi

    if [ -d "$fw_dir/IOKit.framework" ]; then
        pass "IOKit.framework present"
    else
        fail "IOKit.framework not found"
    fi
}

# Check LaunchDaemon plist compatibility
check_launchdaemon() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[LaunchDaemon Support]${NC}"; fi

    LAUNCHD_DIR="$SYSTEM_VOL/System/Library/LaunchDaemons"
    if [ -d "$LAUNCHD_DIR" ]; then
        pass "LaunchDaemons directory exists"

        if ls "$LAUNCHD_DIR"/*.plist >/dev/null 2>&1; then
            # Check multiple plists for KeepAlive support
            local found_keepalive=0
            for plist in "$LAUNCHD_DIR"/*.plist; do
                if grep -q "KeepAlive" "$plist" 2>/dev/null; then
                    found_keepalive=1
                    break
                fi
            done
            if [ "$found_keepalive" -eq 1 ]; then
                pass "KeepAlive key supported (found in system plists)"
            else
                info "KeepAlive key not found in system plists (may still be supported)"
            fi
        fi
    else
        warn "LaunchDaemons directory not found"
    fi
}

# Detect default filesystem type
check_filesystem() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[Filesystem]${NC}"; fi

    if [ -z "$VERSION" ] || [ "$VERSION" = "GM" ]; then
        warn "Cannot determine filesystem (version unknown)"
        return
    fi

    local major minor
    major=$(echo "$VERSION" | cut -d. -f1)
    minor=$(echo "$VERSION" | cut -d. -f2)

    if [ "$major" -gt 10 ] || ([ "$major" -eq 10 ] && [ "$minor" -ge 13 ]); then
        info "Default filesystem: APFS (10.13+)"
        info "Freeze: sync + F_FULLFSYNC + tmutil snapshot"
    else
        info "Default filesystem: HFS+"
        info "Freeze: sync + F_FULLFSYNC only (no APFS snapshots)"
    fi

    if [ -f "$SYSTEM_VOL/usr/sbin/diskutil" ]; then
        if strings "$SYSTEM_VOL/usr/sbin/diskutil" 2>/dev/null | grep -qi "apfs"; then
            pass "diskutil has APFS support"
        else
            info "diskutil does not reference APFS (pre-10.13)"
        fi
    fi
}

# Detect architecture support
check_architecture() {
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo -e "${BOLD}[Architecture]${NC}"; fi

    for binary in "$SYSTEM_VOL/usr/bin/sw_vers" "$SYSTEM_VOL/bin/ls" "$SYSTEM_VOL/usr/bin/true"; do
        if [ -f "$binary" ]; then
            archs=$(file "$binary" 2>/dev/null | grep -oE '(x86_64|i386|arm64|arm64e)' | sort -u | tr '\n' ' ')
            if [ -n "$archs" ]; then
                pass "Supported architectures: $archs"
            fi
            break
        fi
    done

    if [ -z "$VERSION" ] || [ "$VERSION" = "GM" ]; then
        return
    fi

    local major minor
    major=$(echo "$VERSION" | cut -d. -f1)
    minor=$(echo "$VERSION" | cut -d. -f2)

    if [ "$major" -gt 10 ]; then
        info "Agent binary: arm64 (mac-guest-agent-darwin-arm64)"
    elif [ "$major" -eq 10 ] && [ "$minor" -ge 6 ]; then
        info "Agent binary: x86_64 (mac-guest-agent-darwin-amd64)"
    elif [ "$major" -eq 10 ] && [ "$minor" -lt 6 ]; then
        info "Agent binary: i386 (mac-guest-agent-i386, if available)"
    fi
}

# Resolve codename from version
get_codename() {
    local major minor
    major=$(echo "${VERSION:-0}" | cut -d. -f1)
    minor=$(echo "${VERSION:-0}" | cut -d. -f2)

    if [ "$major" -eq 10 ] 2>/dev/null; then
        case "$minor" in
            4) echo "Tiger" ;;       5) echo "Leopard" ;;
            6) echo "Snow Leopard" ;; 7) echo "Lion" ;;
            8) echo "Mountain Lion" ;; 9) echo "Mavericks" ;;
            10) echo "Yosemite" ;;    11) echo "El Capitan" ;;
            12) echo "Sierra" ;;      13) echo "High Sierra" ;;
            14) echo "Mojave" ;;      15) echo "Catalina" ;;
        esac
    else
        case "$major" in
            11) echo "Big Sur" ;;     12) echo "Monterey" ;;
            13) echo "Ventura" ;;     14) echo "Sonoma" ;;
            15) echo "Sequoia" ;;     26) echo "Tahoe" ;;
        esac
    fi
}

# Print JSON output
print_json() {
    local codename verdict
    codename=$(get_codename)
    [ "$FAILS" -eq 0 ] && verdict="pass" || verdict="fail"

    # Escape special chars in VERSION/BUILD
    local v="${VERSION:-unknown}"
    local b="${BUILD:-unknown}"

    printf '{"version":"%s","build":"%s","codename":"%s","status":"%s","passes":%d,"warnings":%d,"failures":%d,"checks":[%s]}\n' \
        "$v" "$b" "$codename" "$verdict" "$PASSES" "$WARNS" "$FAILS" "$JSON_CHECKS"
}

# Print summary for COMPATIBILITY.md
print_summary() {
    if [ "$JSON_OUTPUT" -eq 1 ]; then
        print_json
        return
    fi

    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo "========================================="
    echo -e "${BOLD}Verification Result${NC}"
    echo "========================================="
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo "  $PASSES passed, $WARNS warnings, $FAILS failures"
    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi

    if [ "$FAILS" -eq 0 ]; then
        echo -e "  ${GREEN}VERDICT: Agent should work on this macOS version${NC}"
    elif [ "$FAILS" -le 2 ]; then
        echo -e "  ${YELLOW}VERDICT: Agent likely works (non-critical failures)${NC}"
    else
        echo -e "  ${RED}VERDICT: Agent may not work on this macOS version${NC}"
    fi

    if [ -z "$VERSION" ] || [ "$VERSION" = "GM" ]; then
        echo ""
        fail "Cannot generate compatibility table row (version unknown)"
        return
    fi

    local NAME
    NAME=$(get_codename)

    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo -e "${BOLD}COMPATIBILITY.md row:${NC}"
    echo "| $VERSION ${NAME:+$NAME} | 2 | Installer-verified | Untested | Untested | Untested | Untested | Untested | Deep verify $PASSES/$((PASSES+WARNS+FAILS)) ($(date +%Y-%m-%d)) |"

    if [ "$JSON_OUTPUT" -eq 0 ]; then echo ""; fi
    echo "To upgrade to Tier 1, run the agent in a VM:"
    echo "  1. Install the binary"
    echo "  2. Run: sudo mac-guest-agent --self-test"
    echo "  3. Run: ./tests/safe_test.sh /usr/local/bin/mac-guest-agent"
    echo "  4. Test PVE integration: qm agent <vmid> ping"
    echo "  5. Test freeze: qm guest cmd <vmid> fsfreeze-freeze"
}

# Main
if [ "$JSON_OUTPUT" -eq 0 ]; then
    echo "macOS Installer Verification for Guest Agent Compatibility"
    echo "==========================================================="
fi

resolve_input
detect_version
check_tools
check_serial_kext
check_symbols
check_frameworks
check_launchdaemon
check_filesystem
check_architecture
print_summary
