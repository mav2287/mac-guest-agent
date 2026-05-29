#!/bin/bash
# Coverage for scripts/install.sh argument parsing, mutually-exclusive flag
# rejection, and --dry-run plan output (v2.5.3+).
#
# Does NOT exercise the actual prereq checks (csrutil, sw_vers, lsof,
# launchctl) — those probe the live system and would need PATH-stubbing
# for hermetic tests. Manual verification on a SIP-enabled / pre-Big-Sur
# host covers the refusal paths; this file covers the flag-parsing surface
# that runs regardless of host.

# --- PIPELINE CONVENTION (do not violate) -------------------------------------
# install.sh runs with `set -e` (no pipefail), but this test file sets
# pipefail below for the assertion helpers. Same rule as run_tests.sh:
# no `producer | early-exit-consumer` patterns. Use bash `case` for
# substring assertions instead of `grep -q` on a pipe.
# ------------------------------------------------------------------------------
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="$SCRIPT_DIR/scripts/install.sh"

if [ ! -x "$INSTALL_SH" ] && [ ! -r "$INSTALL_SH" ]; then
    echo "install.sh not found at $INSTALL_SH"
    exit 1
fi

PASS=0
FAIL=0
FAILS=()

pass() { PASS=$((PASS+1)); echo "  PASS  $1"; }
fail() { FAIL=$((FAIL+1)); FAILS+=("$1"); echo "  FAIL  $1"; }

# Run install.sh with the given args, capture combined output and exit code.
# Returns 0 always; output is in $OUT, exit code in $RC.
#
# Tempting to write `OUT=$(...) || true` but the trailing `|| true` makes
# `$?` capture true's exit code (0), erasing the install.sh exit code we
# care about. Instead: keep set -e off (the test runner doesn't enable it),
# let the substitution complete on any exit code, and capture $? directly.
run_install() {
    set +e
    OUT=$(bash "$INSTALL_SH" "$@" 2>&1)
    RC=$?
    set -e
    return 0
}

assert_contains() {
    local label="$1" needle="$2" haystack="$3"
    case "$haystack" in
        *"$needle"*) pass "$label" ;;
        *)           fail "$label (did not contain '$needle')" ;;
    esac
}

assert_not_contains() {
    local label="$1" needle="$2" haystack="$3"
    case "$haystack" in
        *"$needle"*) fail "$label (unexpectedly contained '$needle')" ;;
        *)           pass "$label" ;;
    esac
}

assert_rc() {
    local label="$1" expected="$2" actual="$3"
    if [ "$actual" -eq "$expected" ]; then
        pass "$label (rc=$actual)"
    else
        fail "$label (expected rc=$expected, got $actual)"
    fi
}

echo "=== scripts/install.sh flag-parsing tests ==="

# --help should mention every new flag.
run_install --help
assert_rc           "--help exits 0"                 0 "$RC"
assert_contains     "--help mentions --virtio"       "--virtio        Install with VirtIO transport override" "$OUT"
assert_contains     "--help mentions --virtio-force" "--virtio-force  Advanced: install with VirtIO" "$OUT"
assert_contains     "--help mentions --uninstall"    "--uninstall     Remove mac-guest-agent" "$OUT"
assert_contains     "--help still mentions --local"  "--local         Install from a local binary" "$OUT"
assert_contains     "--help still mentions --dry-run" "--dry-run       Print every action" "$OUT"

# --virtio + --virtio-force is a hard error.
run_install --virtio --virtio-force --dry-run
assert_rc           "--virtio + --virtio-force is rejected"          1 "$RC"
assert_contains     "rejection message names both flags"             "Cannot combine --virtio and --virtio-force" "$OUT"

# --virtio-force + --virtio is the same conflict, opposite order.
run_install --virtio-force --virtio --dry-run
assert_rc           "--virtio-force + --virtio rejected (reverse)"   1 "$RC"

# --uninstall does not combine with --virtio / --virtio-force / --dry-run.
run_install --uninstall --virtio
assert_rc           "--uninstall + --virtio is rejected"             1 "$RC"
assert_contains     "rejection mentions incompatible combination"    "--uninstall does not combine with" "$OUT"

run_install --uninstall --virtio-force
assert_rc           "--uninstall + --virtio-force is rejected"       1 "$RC"

run_install --uninstall --dry-run
assert_rc           "--uninstall + --dry-run is rejected"            1 "$RC"

run_install --uninstall --local /tmp/some-binary
assert_rc           "--uninstall + --local is rejected"              1 "$RC"
assert_contains     "rejection lists --local"                        "--local" "$OUT"

# --dry-run --virtio: prints the gated plan with apple-unload + verify.
run_install --dry-run --virtio
assert_rc           "--dry-run --virtio exits 0"                     0 "$RC"
assert_contains     "plan shows prereq check step"                   "prereq checks: macOS>=11, SIP off" "$OUT"
assert_contains     "plan shows interactive warning + yes/no"        "interactive warning + yes/no via /dev/tty" "$OUT"
assert_contains     "plan shows Apple agent unload"                  "launchctl unload -w /System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist" "$OUT"
assert_contains     "plan shows post-unload verify"                  "verify: launchctl list && lsof on /dev/cu.org.qemu.guest_agent.0" "$OUT"
assert_contains     "plan shows config write"                        "write /etc/qemu/qemu-ga.conf with path = /dev/cu.org.qemu.guest_agent.0" "$OUT"
assert_contains     "plan shows marker drop (mode=full)"             "drop marker /var/db/mac-guest-agent/.virtio-mode (mode=full)" "$OUT"
assert_contains     "plan shows agent functional verify"             "verify: agent running + log shows" "$OUT"

# --dry-run --virtio-force: same install steps but NO Apple unload, NO verify.
run_install --dry-run --virtio-force
assert_rc           "--dry-run --virtio-force exits 0"               0 "$RC"
assert_contains     "force plan shows 'no prereq checks' marker"     "no prereq checks, no Apple agent unload" "$OUT"
assert_not_contains "force plan does NOT include Apple agent unload" "launchctl unload -w /System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist" "$OUT"
assert_not_contains "force plan does NOT include post-unload verify" "verify: launchctl list && lsof" "$OUT"
assert_contains     "force plan shows marker drop (mode=force)"      "drop marker /var/db/mac-guest-agent/.virtio-mode (mode=force)" "$OUT"
assert_not_contains "force plan does NOT include functional verify"  "verify: agent running + log shows" "$OUT"

# --dry-run alone (no virtio flags): standard path unchanged.
run_install --dry-run
assert_rc           "--dry-run (default) exits 0"                    0 "$RC"
assert_contains     "default plan shows serial-device probe"         "serial-device probe" "$OUT"
assert_not_contains "default plan does NOT include Apple agent unload" "launchctl unload -w /System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist" "$OUT"
assert_not_contains "default plan does NOT include override config"  "/etc/qemu/qemu-ga.conf" "$OUT"
assert_not_contains "default plan does NOT include marker file"      ".virtio-mode" "$OUT"

# --dry-run --virtio --local with a missing path is still caught before
# the install plan prints (sanity check that --local validation precedes
# the dry-run plan).
run_install --dry-run --virtio --local /no/such/path/exists
assert_rc           "--virtio + --local with bad path fails"         1 "$RC"
assert_contains     "bad --local path produces a clear error"        "Local binary not found at: /no/such/path/exists" "$OUT"

echo ""
echo "==================================="
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "Failures:"
    for f in "${FAILS[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
echo "Status: ALL CHECKS PASSED"
