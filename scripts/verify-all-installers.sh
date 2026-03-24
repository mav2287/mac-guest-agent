#!/bin/bash
# Batch-verify all macOS installers and produce a comparison matrix
#
# Usage: ./scripts/verify-all-installers.sh /path/to/installer/directory
#
# Produces:
#   - Per-installer deep verification
#   - Cross-version comparison matrix
#   - Kext version timeline
#   - Feature availability timeline

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALLER_DIR="${1:-$HOME/Downloads/OS X Installs}"

if [ ! -d "$INSTALLER_DIR" ]; then
    echo "Installer directory not found: $INSTALLER_DIR"
    echo "Usage: $0 /path/to/installer/directory"
    exit 1
fi

# Colors
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RED='\033[0;31m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    GREEN='' YELLOW='' RED='' BOLD='' NC=''
fi

MOUNTS_TO_DETACH=()
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"; for ((i=${#MOUNTS_TO_DETACH[@]}-1; i>=0; i--)); do hdiutil detach "${MOUNTS_TO_DETACH[$i]}" -quiet 2>/dev/null || true; done' EXIT

mount_dmg() {
    local dmg="$1"
    local mount_out
    mount_out=$(hdiutil attach "$dmg" -nobrowse -readonly 2>&1) || return 1
    local vol
    vol=$(echo "$mount_out" | grep "/Volumes/" | head -1 | awk -F'\t' '{print $NF}' | sed 's/^ *//')
    if [ -n "$vol" ]; then
        MOUNTS_TO_DETACH+=("$vol")
        echo "$vol"
    else
        return 1
    fi
}

detach_all() {
    for ((i=${#MOUNTS_TO_DETACH[@]}-1; i>=0; i--)); do
        hdiutil detach "${MOUNTS_TO_DETACH[$i]}" -quiet 2>/dev/null || true
    done
    MOUNTS_TO_DETACH=()
}

# Resolve an app bundle to its system volume
resolve_to_system_vol() {
    local app="$1"
    local esd_vol=""
    local sys_vol=""

    if [ -f "$app/Contents/SharedSupport/SharedSupport.dmg" ]; then
        esd_vol=$(mount_dmg "$app/Contents/SharedSupport/SharedSupport.dmg") || true
    fi
    if [ -z "$esd_vol" ] && [ -f "$app/Contents/SharedSupport/InstallESD.dmg" ]; then
        esd_vol=$(mount_dmg "$app/Contents/SharedSupport/InstallESD.dmg") || true
    fi
    if [ -z "$esd_vol" ] && [ -f "$app/Contents/SharedSupport/BaseSystem.dmg" ]; then
        sys_vol=$(mount_dmg "$app/Contents/SharedSupport/BaseSystem.dmg") || true
    fi

    if [ -n "$esd_vol" ] && [ -z "$sys_vol" ]; then
        if [ -f "$esd_vol/BaseSystem.dmg" ]; then
            sys_vol=$(mount_dmg "$esd_vol/BaseSystem.dmg") || true
        fi
        if [ -z "$sys_vol" ]; then
            sys_vol="$esd_vol"
        fi
    fi

    echo "$sys_vol"
}

# Extract all data from one installer
analyze_installer() {
    local app="$1"
    local name
    name=$(basename "$app" .app)
    local result_file="$RESULTS_DIR/$name.txt"

    echo "  Analyzing: $name"

    local sys_vol
    sys_vol=$(resolve_to_system_vol "$app")
    if [ -z "$sys_vol" ]; then
        echo "VERSION=unknown" > "$result_file"
        echo "RESOLVE_FAIL=1" >> "$result_file"
        detach_all
        return
    fi

    # Version
    local version="" build=""
    for plist_path in \
        "$sys_vol/System/Library/CoreServices/SystemVersion.plist" \
        "$sys_vol/System Library/CoreServices/SystemVersion.plist"; do
        if [ -f "$plist_path" ]; then
            version=$(/usr/libexec/PlistBuddy -c "Print :ProductVersion" "$plist_path" 2>/dev/null || true)
            build=$(/usr/libexec/PlistBuddy -c "Print :ProductBuildVersion" "$plist_path" 2>/dev/null || true)
            break
        fi
    done
    if [ -z "$version" ] || [ "$version" = "GM" ]; then
        version=$(echo "$name" | grep -oE '10\.[0-9]+|1[1-5]\.[0-9]+' | head -1)
    fi

    {
        echo "VERSION='$version'"
        echo "BUILD='$build'"
        echo "NAME='$name'"

        # Apple16X50Serial.kext
        local kext="$sys_vol/System/Library/Extensions/Apple16X50Serial.kext"
        if [ -d "$kext" ]; then
            echo "SERIAL_KEXT=present"
            local kext_ver
            kext_ver=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$kext/Contents/Info.plist" 2>/dev/null || true)
            echo "SERIAL_KEXT_VERSION='$kext_ver'"
            local class_match
            class_match=$(grep -A1 "IOPCIClassMatch" "$kext/Contents/Info.plist" 2>/dev/null | grep "<string>" | sed 's/.*<string>\(.*\)<\/string>.*/\1/' | sed 's/&amp;/\&/g' || true)
            echo "SERIAL_PCI_CLASS='$class_match'"
        else
            echo "SERIAL_KEXT=missing"
        fi

        # IOSerialFamily.kext
        local ioserial="$sys_vol/System/Library/Extensions/IOSerialFamily.kext"
        if [ -d "$ioserial" ]; then
            echo "IOSERIAL_KEXT=present"
            local ioserial_ver
            ioserial_ver=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$ioserial/Contents/Info.plist" 2>/dev/null || true)
            echo "IOSERIAL_VERSION='$ioserial_ver'"
        else
            echo "IOSERIAL_KEXT=missing"
        fi

        # VirtIO kext
        local virtio_found="missing"
        for vkext in \
            "$sys_vol/System/Library/Extensions/AppleVirtIO.kext" \
            "$sys_vol/System/Library/Extensions/AppleVirtIOStorage.kext" \
            "$sys_vol/System/Library/Extensions/AppleVirtIONetworking.kext"; do
            if [ -d "$vkext" ]; then
                virtio_found="present"
                break
            fi
        done
        echo "VIRTIO_KEXT=$virtio_found"

        # Critical symbols check
        local syslib_dir="$sys_vol/usr/lib/system"
        if [ -d "$syslib_dir" ]; then
            local symfile
            symfile=$(mktemp)
            nm -g "$syslib_dir"/*.dylib 2>/dev/null | grep " T _" | awk '{print $NF}' | sort -u > "$symfile"
            local sym_missing=0
            for sym in _getifaddrs _freeifaddrs _getutxent _endutxent _getloadavg \
                       _getmntinfo _getpwnam _sysctlbyname _gettimeofday _settimeofday \
                       _host_statistics _host_statistics64 _poll _strtok_r _fcntl \
                       _sync _tcgetattr _tcsetattr _tcflush _tcdrain; do
                if ! grep -qx "$sym" "$symfile" 2>/dev/null; then
                    sym_missing=$((sym_missing + 1))
                fi
            done
            rm -f "$symfile"
            echo "SYMBOLS_MISSING=$sym_missing"
        else
            echo "SYMBOLS_MISSING=unknown"
        fi

        # Frameworks
        [ -d "$sys_vol/System/Library/Frameworks/CoreFoundation.framework" ] && echo "COREFOUNDATION=present" || echo "COREFOUNDATION=missing"
        [ -d "$sys_vol/System/Library/Frameworks/IOKit.framework" ] && echo "IOKIT=present" || echo "IOKIT=missing"

        # Tools
        for tool_entry in \
            "sw_vers:usr/bin/sw_vers" \
            "diskutil:usr/sbin/diskutil" \
            "sysctl:usr/sbin/sysctl" \
            "osascript:usr/bin/osascript" \
            "shutdown:sbin/shutdown" \
            "pmset:usr/bin/pmset" \
            "dscl:usr/bin/dscl" \
            "launchctl:bin/launchctl" \
            "tmutil:usr/bin/tmutil" \
            "netstat:usr/sbin/netstat"; do
            IFS=':' read -r tname tpath <<< "$tool_entry"
            [ -f "$sys_vol/$tpath" ] && echo "TOOL_${tname}=present" || echo "TOOL_${tname}=missing"
        done

        # tmutil localsnapshot support (check for subcommand in binary)
        if [ -f "$sys_vol/usr/bin/tmutil" ]; then
            if strings "$sys_vol/usr/bin/tmutil" 2>/dev/null | grep -q "localsnapshot"; then
                echo "TMUTIL_SNAPSHOT=supported"
            else
                echo "TMUTIL_SNAPSHOT=unsupported"
            fi
        else
            echo "TMUTIL_SNAPSHOT=no_tmutil"
        fi

        # Default shell
        if [ -f "$sys_vol/bin/sh" ]; then
            local shell_type
            shell_type=$(file "$sys_vol/bin/sh" 2>/dev/null || true)
            if echo "$shell_type" | grep -q "link\|symbolic"; then
                local shell_target
                shell_target=$(readlink "$sys_vol/bin/sh" 2>/dev/null || true)
                echo "DEFAULT_SHELL='$shell_target'"
            elif strings "$sys_vol/bin/sh" 2>/dev/null | grep -q "zsh"; then
                echo "DEFAULT_SHELL=zsh"
            else
                echo "DEFAULT_SHELL=bash"
            fi
        else
            echo "DEFAULT_SHELL=missing"
        fi

        # Architecture
        local archs=""
        for binary in "$sys_vol/usr/bin/sw_vers" "$sys_vol/bin/ls"; do
            if [ -f "$binary" ]; then
                archs=$(file "$binary" 2>/dev/null | grep -oE '(x86_64|i386|arm64|arm64e)' | sort -u | tr '\n' ',' | sed 's/,$//')
                break
            fi
        done
        echo "ARCHITECTURES='$archs'"

        # LaunchDaemon KeepAlive
        local launchd_dir="$sys_vol/System/Library/LaunchDaemons"
        if [ -d "$launchd_dir" ] && ls "$launchd_dir"/*.plist >/dev/null 2>&1; then
            local ka_found=0
            for plist in "$launchd_dir"/*.plist; do
                if grep -q "KeepAlive" "$plist" 2>/dev/null; then
                    ka_found=1
                    break
                fi
            done
            echo "KEEPALIVE=$ka_found"
        else
            echo "KEEPALIVE=unknown"
        fi

        # dscl stdin support (check for passwd subcommand and stdin handling)
        if [ -f "$sys_vol/usr/bin/dscl" ]; then
            if strings "$sys_vol/usr/bin/dscl" 2>/dev/null | grep -q "passwd\|Password"; then
                echo "DSCL_PASSWD=supported"
            else
                echo "DSCL_PASSWD=unknown"
            fi
        else
            echo "DSCL_PASSWD=no_dscl"
        fi

    } > "$result_file"

    detach_all
}

# Collect sorted result files into an array
get_result_files() {
    RESULT_FILES=()
    while IFS= read -r f; do
        RESULT_FILES+=("$f")
    done < <(find "$RESULTS_DIR" -name "*.txt" -print0 | xargs -0 ls 2>/dev/null | sort)
}

load_result() {
    # Reset all variables before sourcing
    VERSION="" BUILD="" NAME="" SERIAL_KEXT="" SERIAL_KEXT_VERSION="" SERIAL_PCI_CLASS=""
    IOSERIAL_KEXT="" IOSERIAL_VERSION="" VIRTIO_KEXT="" SYMBOLS_MISSING=""
    COREFOUNDATION="" IOKIT="" KEEPALIVE="" TMUTIL_SNAPSHOT="" DEFAULT_SHELL=""
    ARCHITECTURES="" DSCL_PASSWD="" RESOLVE_FAIL=""
    TOOL_sw_vers="" TOOL_diskutil="" TOOL_sysctl="" TOOL_osascript="" TOOL_shutdown=""
    TOOL_pmset="" TOOL_dscl="" TOOL_launchctl="" TOOL_tmutil="" TOOL_netstat=""
    source "$1"
}

# Print comparison matrix
print_matrix() {
    get_result_files

    echo ""
    echo -e "${BOLD}============================================================${NC}"
    echo -e "${BOLD}  Cross-Version Comparison Matrix${NC}"
    echo -e "${BOLD}============================================================${NC}"
    echo ""

    # Header
    printf "%-24s %-8s %-8s %-10s %-8s %-8s %-8s %-10s %-6s\n" \
        "Version" "Serial" "VirtIO" "Symbols" "CF+IOK" "KeepAlv" "Shell" "tmSnapshot" "Arch"
    printf "%-24s %-8s %-8s %-10s %-8s %-8s %-8s %-10s %-6s\n" \
        "------------------------" "--------" "--------" "----------" "--------" "--------" "--------" "----------" "------"

    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"

        local serial_icon virtio_icon sym_icon fw_icon ka_icon snap_icon
        [ "$SERIAL_KEXT" = "present" ] && serial_icon="${GREEN}yes${NC}" || serial_icon="${RED}NO${NC}"
        [ "$VIRTIO_KEXT" = "present" ] && virtio_icon="${GREEN}yes${NC}" || virtio_icon="-"
        [ "$SYMBOLS_MISSING" = "0" ] && sym_icon="${GREEN}20/20${NC}" || sym_icon="${RED}${SYMBOLS_MISSING} miss${NC}"
        [ "$COREFOUNDATION" = "present" ] && [ "$IOKIT" = "present" ] && fw_icon="${GREEN}yes${NC}" || fw_icon="${RED}NO${NC}"
        [ "$KEEPALIVE" = "1" ] && ka_icon="${GREEN}yes${NC}" || ka_icon="${YELLOW}?${NC}"
        [ "$TMUTIL_SNAPSHOT" = "supported" ] && snap_icon="${GREEN}yes${NC}" || snap_icon="-"

        printf "%-24s %-18s %-18s %-20s %-18s %-18s %-8s %-20s %-6s\n" \
            "${VERSION:-?}" "$serial_icon" "$virtio_icon" "$sym_icon" "$fw_icon" "$ka_icon" "${DEFAULT_SHELL:-?}" "$snap_icon" "${ARCHITECTURES:-?}"
    done

    # Kext version timeline
    echo ""
    echo -e "${BOLD}Apple16X50Serial.kext Version Timeline${NC}"
    echo "----------------------------------------------"
    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"
        if [ "$SERIAL_KEXT" = "present" ]; then
            printf "  %-20s  kext v%-10s  PCI: %s\n" "${VERSION:-?}" "${SERIAL_KEXT_VERSION:-?}" "${SERIAL_PCI_CLASS:-?}"
        fi
    done

    # IOSerialFamily version timeline
    echo ""
    echo -e "${BOLD}IOSerialFamily.kext Version Timeline${NC}"
    echo "----------------------------------------------"
    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"
        if [ "$IOSERIAL_KEXT" = "present" ]; then
            printf "  %-20s  v%s\n" "${VERSION:-?}" "${IOSERIAL_VERSION:-?}"
        fi
    done

    # Tool availability matrix
    echo ""
    echo -e "${BOLD}Tool Availability${NC}"
    printf "%-24s " "Version"
    for t in sw_vers diskutil sysctl osascript shutdown pmset dscl launchctl tmutil netstat; do
        printf "%-10s " "$t"
    done
    echo ""
    printf "%-24s " "------------------------"
    for t in sw_vers diskutil sysctl osascript shutdown pmset dscl launchctl tmutil netstat; do
        printf "%-10s " "----------"
    done
    echo ""

    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"
        printf "%-24s " "${VERSION:-?}"
        for t in sw_vers diskutil sysctl osascript shutdown pmset dscl launchctl tmutil netstat; do
            local varname="TOOL_${t}"
            local val="${!varname}"
            if [ "$val" = "present" ]; then
                printf "${GREEN}%-10s${NC} " "yes"
            else
                printf "${YELLOW}%-10s${NC} " "-"
            fi
        done
        echo ""
    done

    # Verdict summary
    echo ""
    echo -e "${BOLD}Verdict Summary${NC}"
    echo "----------------------------------------------"
    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"
        local verdict
        if [ "$SERIAL_KEXT" = "present" ] && [ "$SYMBOLS_MISSING" = "0" ] && \
           [ "$COREFOUNDATION" = "present" ] && [ "$IOKIT" = "present" ]; then
            verdict="${GREEN}SHOULD WORK${NC}"
        elif [ "${RESOLVE_FAIL:-0}" = "1" ]; then
            verdict="${RED}COULD NOT ANALYZE${NC}"
        else
            verdict="${YELLOW}NEEDS INVESTIGATION${NC}"
        fi
        printf "  %-24s  %b\n" "${VERSION:-?}" "$verdict"
    done

    # COMPATIBILITY.md rows
    echo ""
    echo -e "${BOLD}Rows for docs/COMPATIBILITY.md${NC}"
    echo "----------------------------------------------"
    for result in "${RESULT_FILES[@]}"; do
        load_result "$result"
        local major minor codename=""
        major=$(echo "${VERSION:-0}" | cut -d. -f1)
        minor=$(echo "${VERSION:-0}" | cut -d. -f2)

        if [ "$major" -eq 10 ] 2>/dev/null; then
            case "$minor" in
                7) codename="Lion" ;; 8) codename="Mountain Lion" ;; 9) codename="Mavericks" ;;
                10) codename="Yosemite" ;; 11) codename="El Capitan" ;; 12) codename="Sierra" ;;
                13) codename="High Sierra" ;; 14) codename="Mojave" ;; 15) codename="Catalina" ;;
            esac
        else
            case "$major" in
                11) codename="Big Sur" ;; 12) codename="Monterey" ;; 13) codename="Ventura" ;; 14) codename="Sonoma" ;;
            esac
        fi

        local score=0 total=0
        [ "$SERIAL_KEXT" = "present" ] && score=$((score+1)); total=$((total+1))
        [ "$SYMBOLS_MISSING" = "0" ] && score=$((score+1)); total=$((total+1))
        [ "$COREFOUNDATION" = "present" ] && score=$((score+1)); total=$((total+1))
        [ "$IOKIT" = "present" ] && score=$((score+1)); total=$((total+1))

        echo "| ${VERSION:-?} ${codename:+$codename} | 2 | Installer verified | Untested | Untested | Untested | Untested | Untested | Deep verify $score/$total ($(date +%Y-%m-%d)) |"
    done
}

# Main
echo -e "${BOLD}macOS Installer Batch Verification${NC}"
echo "==========================================================="
echo "Installer directory: $INSTALLER_DIR"
echo ""

# Find all .app installers
count=0
for app in "$INSTALLER_DIR"/*.app; do
    [ -d "$app" ] || continue
    # Skip incomplete downloads (less than 100MB)
    local_size=$(du -sm "$app" 2>/dev/null | awk '{print $1}')
    if [ "${local_size:-0}" -lt 100 ]; then
        echo "  SKIP: $(basename "$app") (${local_size}MB — likely still downloading)"
        continue
    fi
    analyze_installer "$app"
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "No complete installer .app bundles found in: $INSTALLER_DIR"
    exit 1
fi

echo ""
echo "Analyzed $count installer(s)"

print_matrix
