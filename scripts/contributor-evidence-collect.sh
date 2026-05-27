#!/bin/bash
# contributor-evidence-collect.sh
# ================================
#
# In-guest helper for collecting Tier-2 runtime evidence for a macOS
# version we haven't fully validated in CI. Designed to run on the OLD
# macOS guest itself, not from a host — which means it has to work on
# Mac OS X 10.4 Tiger with /bin/bash (and not require curl/HTTPS,
# Python 3, jq, or anything Apple shipped after 2010).
#
# What it captures (without QGA host channel):
#   - sw_vers + uname for OS identity
#   - file + lipo -info for the binary's Mach-O structure
#   - shasum -a 256 for tamper check vs published asset
#   - --version stamp
#   - --self-test-json (or --self-test on Tiger which lacks --self-test-json)
#   - launchctl list for the installed daemon
#   - tail of the agent log
#
# What it deliberately does NOT do:
#   - Run freeze/thaw cycles (those need a host-side QGA channel via
#     scripts/verify.sh)
#   - Touch /usr/local/bin or /Library/LaunchDaemons (it's a read-only
#     evidence collector; the user should have already installed the
#     agent per docs/TESTING_HARNESS.md before running this)
#   - Phone home or upload anything (output goes to stdout + a single
#     file the user can transfer back to a modern machine for PR
#     submission)
#
# Usage:
#   sudo ./contributor-evidence-collect.sh [BINARY_PATH] [OUTPUT_FILE]
#
#   BINARY_PATH defaults to /usr/local/bin/mac-guest-agent (where
#   --install puts it). If you want to evidence-test before install,
#   pass the path to the universal binary directly.
#
#   OUTPUT_FILE defaults to /tmp/mac-guest-agent-evidence.txt. The
#   file is also printed to stdout. Transfer to a modern machine via
#   scp/USB/shared folder and submit per docs/evidence/README.md.
#
# Exit code: 0 on a successful capture (even if the agent has warnings
# or isn't running); non-zero only if the binary itself is unusable
# (not found, not executable, won't print --version).

BINARY="${1:-/usr/local/bin/mac-guest-agent}"
OUTPUT="${2:-/tmp/mac-guest-agent-evidence.txt}"

# No `set -e` — we deliberately want to keep collecting if a single
# probe fails on the older OS (e.g. --self-test-json not present on
# pre-2.4 agents). Each command's failure is recorded in the output.

# Pretty banner to stdout so the contributor sees what's happening.
emit() {
    printf '%s\n' "$*"
}

probe() {
    local name="$1"
    shift
    emit ""
    emit "--- ${name} ---"
    emit "\$ $*"
    "$@" 2>&1
    emit "(exit code: $?)"
}

# All capture goes to OUTPUT and stdout simultaneously.
{
    emit "============================================================"
    emit " mac-guest-agent contributor evidence collection"
    emit " Generated: $(date '+%Y-%m-%dT%H:%M:%S%z' 2>/dev/null || date)"
    emit " Hostname:  $(hostname 2>/dev/null || echo unknown)"
    emit "============================================================"

    emit ""
    emit "Note: this is an IN-GUEST evidence drop. Hostnames and"
    emit "IP addresses are NOT auto-redacted here — review before"
    emit "submitting if your guest has anything sensitive in its"
    emit "hostname or environment. The host-side scripts/verify.sh"
    emit "redacts automatically; this script doesn't because it has"
    emit "to run on Tiger/Leopard where the perl/regex surface is"
    emit "older and the helper script for redaction isn't trivial"
    emit "to ship. See docs/TESTING_HARNESS.md for the redaction"
    emit "checklist before opening a PR."

    probe "OS identity (sw_vers)"     sw_vers
    probe "Kernel + uname"            uname -a
    probe "Hardware model + CPU"      sysctl -n hw.model hw.ncpu hw.memsize machdep.cpu.brand_string

    emit ""
    emit "============================================================"
    emit " Binary structural checks (loader-safe — does NOT run main())"
    emit "============================================================"

    if [ ! -e "$BINARY" ]; then
        emit ""
        emit "FATAL: binary not found at: $BINARY"
        emit "Pass a path: sudo $0 /path/to/mac-guest-agent-darwin-universal"
        exit 1
    fi

    probe "ls -la BINARY"             ls -la "$BINARY"
    probe "file BINARY"               file "$BINARY"
    probe "lipo -info BINARY"         lipo -info "$BINARY"
    # lipo -detailed_info is more useful but verbose; included for
    # forensic completeness because if this runs at all on a target OS
    # we want every byte of context for post-mortem.
    probe "lipo -detailed_info"       lipo -detailed_info "$BINARY"

    # Tamper / supply-chain check: did the user transfer the binary
    # without corruption?
    probe "shasum -a 256 BINARY"      shasum -a 256 "$BINARY"

    emit ""
    emit "============================================================"
    emit " Runtime probes (these execute main(); proves dyld loaded"
    emit " a slice successfully — the real LC_MAIN/LC_UNIXTHREAD test)"
    emit "============================================================"

    if [ ! -x "$BINARY" ]; then
        emit ""
        emit "WARN: binary not executable: $BINARY"
        emit "      Try: sudo chmod +x $BINARY"
        emit "      Skipping runtime probes."
    else
        probe "BINARY --version"      "$BINARY" --version
        probe "BINARY --self-test"    "$BINARY" --self-test
        probe "BINARY --self-test-json (may not exist on pre-2.4 agents)" \
                                       "$BINARY" --self-test-json
    fi

    emit ""
    emit "============================================================"
    emit " LaunchDaemon state (if --install was run)"
    emit "============================================================"

    probe "launchctl list com.macos.guest-agent" \
                                       launchctl list com.macos.guest-agent
    probe "ls /Library/LaunchDaemons/com.macos.guest-agent.plist" \
                                       ls -la /Library/LaunchDaemons/com.macos.guest-agent.plist

    emit ""
    emit "============================================================"
    emit " Agent log tail (last 40 lines)"
    emit "============================================================"

    probe "tail -40 /var/log/mac-guest-agent.log" \
                                       tail -40 /var/log/mac-guest-agent.log

    emit ""
    emit "============================================================"
    emit " Capture complete"
    emit "============================================================"
    emit ""
    emit "Output file: $OUTPUT"
    emit "Transfer this to a modern machine for review, then submit"
    emit "per docs/evidence/README.md or attach to a GitHub issue."

} | tee "$OUTPUT"

# tee preserves the inner shell's exit code only in some shells; force 0
# unless the binary check above explicitly exit'd 1.
exit 0
