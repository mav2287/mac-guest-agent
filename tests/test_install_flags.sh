#!/bin/bash
# Install-flow flag coverage (v2.5.3+).
#
# v2.5.3 moved the install/upgrade state machine OUT of scripts/install.sh
# INTO the binary itself (src/service.c). install.sh is now a slim
# bootstrap wrapper. Tests here cover BOTH:
#
#  1. Binary flag handling — refusal logic, mutex checks, detection-driven
#     routing, --upgrade error paths. Exercised via the binary's --dry-run
#     mode and the MAC_GUEST_AGENT_TEST_STATE / _CONFIG_EXISTS env hooks.
#
#  2. Wrapper smoke — install.sh --help, --local path resolution, --dry-run
#     output shape. The wrapper has very little logic; just confirm it
#     parses and forwards correctly.
#
# Live operations (csrutil, launchctl unload, lsof, /dev/tty prompt) are
# not exercised here — those require a SIP-disabled Big Sur+ VM. Manual
# verification on such a VM covers the live-probe paths; this file covers
# what can run hermetically.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="$SCRIPT_DIR/scripts/install.sh"
BINARY="${BINARY:-$SCRIPT_DIR/build/mac-guest-agent}"

if [ ! -x "$BINARY" ]; then
    echo "Binary not found at $BINARY — run 'make build' first."
    exit 1
fi

PASS=0
FAIL=0
FAILS=()

pass() { PASS=$((PASS+1)); echo "  PASS  $1"; }
fail() { FAIL=$((FAIL+1)); FAILS+=("$1"); echo "  FAIL  $1"; }

# Run a command, capture combined output + exit code.
# Tempting to write `OUT=$(...) || true` but the trailing `|| true` makes
# `$?` capture true's exit code (0), erasing the actual exit code we care
# about. Use `set +e` instead.
run_cmd() {
    set +e
    OUT=$("$@" 2>&1)
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

# =========================================================================
echo "=== Binary: --virtio / --virtio-force / --upgrade flag mutex ==="

run_cmd "$BINARY" --virtio --virtio-force
assert_rc       "--virtio + --virtio-force is rejected"     1 "$RC"
assert_contains "rejection names both flags"                "--virtio and --virtio-force cannot combine" "$OUT"

run_cmd "$BINARY" --virtio
assert_rc       "--virtio alone (without --install) refused" 1 "$RC"
assert_contains "rejection points at --install --virtio"     "modifier for --install" "$OUT"

run_cmd "$BINARY" --virtio-force
assert_rc       "--virtio-force alone refused"              1 "$RC"

run_cmd "$BINARY" --upgrade --install
assert_rc       "--upgrade + --install rejected"            1 "$RC"
assert_contains "rejection names upgrade-vs-install"        "--upgrade cannot combine with --install" "$OUT"

run_cmd "$BINARY" --upgrade --uninstall
assert_rc       "--upgrade + --uninstall rejected"          1 "$RC"

run_cmd "$BINARY" --upgrade --update /tmp/bar
assert_rc       "--upgrade + --update rejected"             1 "$RC"

# =========================================================================
echo "=== Binary: --install --virtio refusals (detection-driven) ==="

# Fresh-install state: --install --virtio proceeds past detection but fails
# the binary-existence check OR a prereq. We just want to confirm refusal
# does NOT fire (no "existing install detected" message). For the actual
# prereq failures (SIP, macOS version, Apple agent presence, VirtIO device)
# we can't test live without the right host.
run_cmd env MAC_GUEST_AGENT_TEST_STATE=standard "$BINARY" --install --virtio --dry-run
assert_rc       "--install --virtio with standard install: refuse" 1 "$RC"
assert_contains "rejection names existing install"          "existing install detected" "$OUT"
assert_contains "rejection points at --upgrade"             "--upgrade" "$OUT"

run_cmd env MAC_GUEST_AGENT_TEST_STATE=virtio-full "$BINARY" --install --virtio --dry-run
assert_rc       "--install --virtio with virtio-full: refuse" 1 "$RC"

run_cmd env MAC_GUEST_AGENT_TEST_STATE=virtio-force "$BINARY" --install --virtio --dry-run
assert_rc       "--install --virtio with virtio-force: refuse" 1 "$RC"

run_cmd env MAC_GUEST_AGENT_TEST_STATE=standard "$BINARY" --install --virtio-force --dry-run
assert_rc       "--install --virtio-force with standard: refuse" 1 "$RC"

# Operator-config refusal (state=not-installed but /etc/qemu/qemu-ga.conf present)
run_cmd env MAC_GUEST_AGENT_TEST_STATE=not-installed MAC_GUEST_AGENT_TEST_CONFIG_EXISTS=1 \
    "$BINARY" --install --virtio --dry-run
assert_rc       "--install --virtio with operator config: refuse" 1 "$RC"
assert_contains "rejection names config path"               "/etc/qemu/qemu-ga.conf" "$OUT"
assert_contains "rejection suggests backup-and-retry"       "back up the existing config" "$OUT"
assert_contains "rejection mentions --virtio-force DIY"     "DIY path" "$OUT"

# =========================================================================
echo "=== Binary: --upgrade error paths ==="

run_cmd env MAC_GUEST_AGENT_TEST_STATE=not-installed "$BINARY" --upgrade --dry-run
assert_rc       "--upgrade with no install: refuse"         1 "$RC"
assert_contains "rejection mentions no install detected"    "no existing install detected" "$OUT"

# --upgrade with standard install: self-sources and gets to the upgrade plan.
run_cmd env MAC_GUEST_AGENT_TEST_STATE=standard "$BINARY" --upgrade --dry-run
assert_rc       "--upgrade with standard install: proceeds" 0 "$RC"
assert_contains "logs self-source path"                     "using self as source" "$OUT"
assert_contains "dry-run shows backup step"                 "backup /usr/local/bin/mac-guest-agent" "$OUT"

# =========================================================================
echo "=== Binary: --update prints deprecation, delegates to --upgrade ==="

run_cmd env MAC_GUEST_AGENT_TEST_STATE=standard "$BINARY" --update /tmp/nonexistent --dry-run
assert_contains "--update prints deprecation notice"        "deprecated in v2.5.3+" "$OUT"
assert_contains "--update points at --upgrade"              "--upgrade" "$OUT"

# =========================================================================
echo "=== Binary: --help mentions new flags ==="

run_cmd "$BINARY" --help
assert_rc       "--help exits 0"                            0 "$RC"
assert_contains "--help mentions --virtio"                  "--virtio           Modifier for --install" "$OUT"
assert_contains "--help mentions --virtio-force"            "--virtio-force     Modifier for --install" "$OUT"
assert_contains "--help mentions --upgrade"                 "--upgrade          In-place upgrade using the running binary as source" "$OUT"
assert_contains "--help marks --update deprecated"          "--update PATH      DEPRECATED" "$OUT"

# =========================================================================
echo "=== install.sh wrapper: smoke tests ==="

run_cmd bash "$INSTALL_SH" --help
assert_rc       "wrapper --help exits 0"                    0 "$RC"
assert_contains "wrapper --help describes its role"         "bootstrap wrapper" "$OUT"
assert_contains "wrapper --help mentions --virtio"          "--virtio" "$OUT"
assert_contains "wrapper --help mentions --upgrade"         "--upgrade" "$OUT"
assert_contains "wrapper --help mentions --uninstall"       "--uninstall" "$OUT"

# Wrapper rejects unknown architectures via arch check (we can't easily
# force a fake uname, but the --help path doesn't require root so we
# confirm --help is the early-exit path).

# Wrapper --local with bad path bails clean.
run_cmd bash "$INSTALL_SH" --dry-run --local /no/such/path
assert_rc       "wrapper --local with bad path fails"       1 "$RC"
assert_contains "bad --local path produces clear error"     "Local binary not found at: /no/such/path" "$OUT"

# =========================================================================
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
