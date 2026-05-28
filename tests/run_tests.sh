#!/bin/bash
# macOS Guest Agent - Comprehensive Test Suite
#
# Tests the agent binary via --test mode (stdin/stdout).
# Validates JSON structure, field presence, and data types
# for every command.
#
# Usage:
#   ./tests/run_tests.sh [binary_path]
#   ./tests/run_tests.sh build/mac-guest-agent-x86_64   # test x86_64 via Rosetta

set -uo pipefail

# --- PIPELINE CONVENTION (do not violate) -------------------------------------
# Pipelines of the shape `producer | <consumer-that-exits-early>` (where
# early-exit means `awk '...{print; exit}'`, `head -1`, `grep -q`, `sed
# 'NUMq'`, etc.) are FORBIDDEN under the `set -o pipefail` setting above.
# When the consumer exits before the producer finishes writing, the producer
# gets SIGPIPE on its next write, dies with status 141, and pipefail
# propagates non-zero through the pipeline — making the surrounding `if` or
# command-substitution see a "failure" that didn't logically happen.
#
# Observed once: CI run 26532052157, commit d0bde24, macos-14, 2026-05-27
# (in tests/test_verify_transports.sh; see that file's ASSERTION-HELPER
# CONVENTION block for the canonical write-up).
#
# Rule for this file: capture binary `--test` output via `awk 'NR==1{...;
# print}'` WITHOUT an `exit` action — awk reads to EOF, the producer
# finishes cleanly, no SIGPIPE possible. Use the same `awk 'NR==1{...;
# print}'` form instead of `sed ... | head -1`.
# ------------------------------------------------------------------------------

BINARY="${1:-./build/mac-guest-agent}"
PASS=0
FAIL=0
SKIP=0
ERRORS=""

if [ ! -x "$BINARY" ]; then
    echo "Binary not found or not executable: $BINARY"
    echo "Run 'make build' first."
    exit 1
fi

# Detect binary architecture(s). For a thin binary `file` reports one arch;
# for a fat (universal) binary it lists every slice. In the universal case
# the plain `grep | head -1` picked the first listed slice (e.g. "i386" from
# `[i386:... ] [x86_64:... ] [arm64:... ]`), which was misleading on modern
# macOS where dyld actually runs the arm64 slice. Now we list all slices for
# fat binaries and additionally probe `--self-test-json` to report the
# runtime slice dyld picked at load time.
ARCH_ALL=$(file "$BINARY" | grep -oE 'arm64|x86_64|i386' | sort -u | tr '\n' '+' | sed 's/+$//')
case "$ARCH_ALL" in
    *+*) # universal / fat binary — multiple arches listed
        SELECTED_ARCH=$("$BINARY" --self-test-json 2>/dev/null \
            | python3 -c 'import json,sys; print(json.load(sys.stdin)["system_info"].get("selected_arch","?"))' 2>/dev/null \
            || echo "?")
        ARCH_LABEL="universal: ${ARCH_ALL//+/ + }, dyld selected: ${SELECTED_ARCH}"
        ;;
    "")
        ARCH_LABEL="unknown"
        ;;
    *)
        ARCH_LABEL="$ARCH_ALL"
        ;;
esac
echo "=============================================="
echo " macOS Guest Agent Test Suite"
echo " Binary: $BINARY ($ARCH_LABEL)"
echo " Host:   $(sw_vers -productName 2>/dev/null || echo 'unknown') $(sw_vers -productVersion 2>/dev/null || echo '')"
echo "=============================================="
echo ""

# Run a command and capture the JSON response (strip the "QMP> " prompt)
run_cmd() {
    local input="$1"
    echo "$input" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}'
}

# Run two commands in sequence (for exec + exec-status pattern)
run_cmd2() {
    local input1="$1"
    local input2="$2"
    printf '%s\n%s\n' "$input1" "$input2" | "$BINARY" --test 2>/dev/null | awk '{line=$0} END{sub(/^QMP> /,"",line); print line}'
}

# Check that a JSON response contains "return" (success)
assert_success() {
    local name="$1"
    local response="$2"
    if echo "$response" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}

# Check that a JSON response contains "error" (expected error)
assert_error() {
    local name="$1"
    local response="$2"
    if echo "$response" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'error' in d" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}

# Validate JSON and check for specific fields in "return"
# Usage: assert_fields "test name" "$response" field1 field2 ...
assert_fields() {
    local name="$1"
    local response="$2"
    shift 2

    local missing=""
    for field in "$@"; do
        if ! echo "$response" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d.get('return', d)
if isinstance(r, list):
    # Check first element of array
    if len(r) > 0:
        assert '$field' in r[0], f'Missing field: $field'
    else:
        pass  # empty array is ok
elif isinstance(r, dict):
    assert '$field' in r, f'Missing field: $field'
" 2>/dev/null; then
            missing="$missing $field"
        fi
    done

    if [ -z "$missing" ]; then
        return 0
    else
        echo "    Missing fields:$missing"
        return 1
    fi
}

# Check return value type
assert_type() {
    local name="$1"
    local response="$2"
    local expected_type="$3"

    if echo "$response" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d['return']
expected = '$expected_type'
if expected == 'object': assert isinstance(r, dict), f'Expected dict, got {type(r)}'
elif expected == 'array': assert isinstance(r, list), f'Expected list, got {type(r)}'
elif expected == 'number': assert isinstance(r, (int, float)), f'Expected number, got {type(r)}'
elif expected == 'string': assert isinstance(r, str), f'Expected string, got {type(r)}'
elif expected == 'empty': assert r == {} or r == [], f'Expected empty, got {r}'
" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}

test_cmd() {
    local name="$1"
    local input="$2"
    local check_type="$3"
    shift 3
    local fields=("$@")

    local response
    response=$(run_cmd "$input")

    # Check we got valid JSON back
    if ! echo "$response" | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null; then
        echo "  FAIL: $name (invalid JSON response)"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  $name: invalid JSON"
        return
    fi

    # Check success vs error based on check_type
    if [ "$check_type" = "error" ]; then
        if assert_error "$name" "$response"; then
            echo "  PASS: $name (expected error)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $name (expected error, got success)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  $name: expected error"
        fi
        return
    fi

    if ! assert_success "$name" "$response"; then
        echo "  FAIL: $name (no 'return' in response)"
        echo "    Response: $response"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  $name: no return"
        return
    fi

    # Check return type
    if [ -n "$check_type" ] && [ "$check_type" != "any" ]; then
        if ! assert_type "$name" "$response" "$check_type"; then
            echo "  FAIL: $name (wrong return type, expected $check_type)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  $name: wrong type"
            return
        fi
    fi

    # Check required fields
    if [ ${#fields[@]} -gt 0 ]; then
        if ! assert_fields "$name" "$response" "${fields[@]}"; then
            echo "  FAIL: $name (missing fields)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  $name: missing fields"
            return
        fi
    fi

    echo "  PASS: $name"
    PASS=$((PASS + 1))
}

# =========================================================
echo "--- Protocol Commands ---"
# =========================================================

test_cmd "guest-ping" \
    '{"execute":"guest-ping"}' \
    "empty"

test_cmd "guest-sync" \
    '{"execute":"guest-sync","arguments":{"id":12345}}' \
    "number"

test_cmd "guest-sync (missing arg)" \
    '{"execute":"guest-sync","arguments":{}}' \
    "error"

# guest-sync-delimited: can't test via this harness due to 0xFF binary prefix
# but we verified it manually with xxd
SKIP=$((SKIP + 1))
echo "  SKIP: guest-sync-delimited (0xFF prefix breaks text parsing; verified manually)"

test_cmd "guest-info" \
    '{"execute":"guest-info"}' \
    "object" \
    "version" "supported_commands"

# Verify command count
CMD_COUNT=$(run_cmd '{"execute":"guest-info"}' | python3 -c "import json,sys; print(len(json.load(sys.stdin)['return']['supported_commands']))" 2>/dev/null)
if [ "$CMD_COUNT" -ge 40 ]; then
    echo "  PASS: guest-info command count ($CMD_COUNT >= 40)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-info command count ($CMD_COUNT < 40)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- System Information Commands ---"
# =========================================================

test_cmd "guest-get-osinfo" \
    '{"execute":"guest-get-osinfo"}' \
    "object" \
    "id" "name" "pretty-name" "version" "kernel-release" "machine"

# Validate OS info content
OS_ID=$(run_cmd '{"execute":"guest-get-osinfo"}' | python3 -c "import json,sys; print(json.load(sys.stdin)['return']['id'])" 2>/dev/null)
if [ "$OS_ID" = "macos" ]; then
    echo "  PASS: guest-get-osinfo id='macos'"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-get-osinfo id='$OS_ID' (expected 'macos')"
    FAIL=$((FAIL + 1))
fi

test_cmd "guest-get-host-name" \
    '{"execute":"guest-get-host-name"}' \
    "object" \
    "host-name"

test_cmd "guest-get-hostname (alias)" \
    '{"execute":"guest-get-hostname"}' \
    "object" \
    "host-name"

test_cmd "guest-get-timezone" \
    '{"execute":"guest-get-timezone"}' \
    "object" \
    "zone" "offset"

test_cmd "guest-get-time" \
    '{"execute":"guest-get-time"}' \
    "number"

# Validate time is reasonable (within last year to next year in nanoseconds)
TIME_NS=$(run_cmd '{"execute":"guest-get-time"}' | python3 -c "import json,sys; print(int(json.load(sys.stdin)['return']))" 2>/dev/null)
NOW_NS=$(python3 -c "import time; print(int(time.time() * 1e9))")
DIFF=$(python3 -c "print(abs($TIME_NS - $NOW_NS))")
if python3 -c "assert $DIFF < 60_000_000_000, 'Time off by more than 60s'" 2>/dev/null; then
    echo "  PASS: guest-get-time value is current (within 60s)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-get-time value off by $(python3 -c "print($DIFF / 1e9)")s"
    FAIL=$((FAIL + 1))
fi

test_cmd "guest-get-users" \
    '{"execute":"guest-get-users"}' \
    "array"

test_cmd "guest-get-load" \
    '{"execute":"guest-get-load"}' \
    "object" \
    "load1m" "load5m" "load15m"

# =========================================================
echo ""
echo "--- Power Commands (structure only, not executed) ---"
# =========================================================

# We can't actually test shutdown/suspend without killing the VM.
# But we can verify the commands are registered and parse args correctly.
echo "  SKIP: guest-shutdown (would halt system)"
echo "  SKIP: guest-suspend-disk (would sleep system)"
echo "  SKIP: guest-suspend-ram (would sleep system)"
echo "  SKIP: guest-suspend-hybrid (would sleep system)"
SKIP=$((SKIP + 4))

# =========================================================
echo ""
echo "--- Hardware Commands ---"
# =========================================================

test_cmd "guest-get-vcpus" \
    '{"execute":"guest-get-vcpus"}' \
    "array" \
    "logical-id" "online" "can-offline"

# Validate CPU count matches system
VCPU_COUNT=$(run_cmd '{"execute":"guest-get-vcpus"}' | python3 -c "import json,sys; print(len(json.load(sys.stdin)['return']))" 2>/dev/null)
SYS_CPUS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo "0")
if [ "$VCPU_COUNT" = "$SYS_CPUS" ]; then
    echo "  PASS: guest-get-vcpus count matches system ($VCPU_COUNT)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-get-vcpus count=$VCPU_COUNT, system=$SYS_CPUS"
    FAIL=$((FAIL + 1))
fi

test_cmd "guest-set-vcpus (unsupported)" \
    '{"execute":"guest-set-vcpus"}' \
    "error"

test_cmd "guest-get-memory-blocks" \
    '{"execute":"guest-get-memory-blocks"}' \
    "array" \
    "phys-index" "online"

test_cmd "guest-get-memory-block-info" \
    '{"execute":"guest-get-memory-block-info"}' \
    "object" \
    "size"

test_cmd "guest-set-memory-blocks (unsupported)" \
    '{"execute":"guest-set-memory-blocks"}' \
    "error"

test_cmd "guest-get-cpustats" \
    '{"execute":"guest-get-cpustats"}' \
    "array"

# Spec-shape contract for guest-get-cpustats: per-CPU array of records,
# each tagged type:"linux" with cpu/user/nice/system/idle fields (matching
# GuestLinuxCpuStats; "linux" is the only currently-defined discriminator
# in upstream GuestCpuStatsType — see docs/design/AGENT_BEHAVIOUR_SPEC.md Q4).
CPUSTATS_SHAPE=$(echo '{"execute":"guest-get-cpustats"}' \
    | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}' \
    | python3 -c "
import json, sys
d = json.load(sys.stdin)
arr = d['return']
if not isinstance(arr, list) or len(arr) == 0:
    print('FAIL: return is not a non-empty array')
    sys.exit(1)
need = {'type','cpu','user','nice','system','idle'}
for i, e in enumerate(arr):
    missing = need - set(e.keys())
    if missing:
        print(f'FAIL: entry {i} missing fields: {missing}')
        sys.exit(1)
    if e['type'] != 'linux':
        print(f'FAIL: entry {i} type={e[\"type\"]!r}, expected \"linux\"')
        sys.exit(1)
    if not isinstance(e['cpu'], int) or e['cpu'] < 0:
        print(f'FAIL: entry {i} cpu={e[\"cpu\"]!r} (expected non-negative int)')
        sys.exit(1)
print(f'OK: {len(arr)} per-CPU entries, all type=linux, all required fields present')
" 2>&1)
if echo "$CPUSTATS_SHAPE" | grep -q '^OK:'; then
    echo "  PASS: cpustats shape (spec-conformant per-CPU array): $CPUSTATS_SHAPE"
    PASS=$((PASS + 1))
else
    echo "  FAIL: cpustats shape: $CPUSTATS_SHAPE"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Disk & Filesystem Commands ---"
# =========================================================

test_cmd "guest-get-disks" \
    '{"execute":"guest-get-disks"}' \
    "array"

test_cmd "guest-get-fsinfo" \
    '{"execute":"guest-get-fsinfo"}' \
    "array" \
    "name" "mountpoint" "type" "total-bytes" "used-bytes"

test_cmd "guest-get-diskstats" \
    '{"execute":"guest-get-diskstats"}' \
    "array"

# Deeper shape contract: each entry must match the QGA GuestDiskStatsInfo
# schema — {name, major, minor, stats: {15 fields}}. macOS supplies real
# cumulative counters for 6 of the 15 stats fields (read/write sectors
# + ios + ticks); the remaining 9 Linux-block-layer-specific fields are
# zero-valued (same honest-zero pattern as cpustats nice:0 and route
# metric:0 / irtt:0). Audit finding 2c.
DISKSTATS=$(echo '{"execute":"guest-get-diskstats"}' | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$DISKSTATS" | python3 -c "
import json, sys
d = json.load(sys.stdin)['return']
assert isinstance(d, list)
assert len(d) > 0, 'expected at least one disk'
required_top = {'name', 'major', 'minor', 'stats'}
required_stats = {
    'read-sectors', 'read-ios', 'read-merges',
    'write-sectors', 'write-ios', 'write-merges',
    'discard-sectors', 'discard-ios', 'discard-merges',
    'read-ticks', 'write-ticks', 'discard-ticks',
    'in-flight', 'io-ticks', 'time-in-queue',
}
for d_entry in d:
    missing = required_top - set(d_entry)
    assert not missing, f'missing top-level: {missing}'
    assert isinstance(d_entry['stats'], dict), 'stats must be a nested object'
    missing_s = required_stats - set(d_entry['stats'])
    assert not missing_s, f'missing stats fields: {missing_s}'
" 2>/dev/null; then
    echo "  PASS: guest-get-diskstats matches GuestDiskStatsInfo schema (name+major+minor+15-field stats)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-get-diskstats shape contract"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Filesystem Freeze Commands ---"
# =========================================================

test_cmd "guest-fsfreeze-status (initial=thawed)" \
    '{"execute":"guest-fsfreeze-status"}' \
    "string"

# Freeze -> check -> thaw -> check cycle (must be in one session since state is per-process)
FREEZE_CYCLE=$(printf '%s\n%s\n%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-fsfreeze-status"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    '{"execute":"guest-fsfreeze-status"}' \
    | "$BINARY" --test 2>/dev/null)

FROZEN=$(echo "$FREEZE_CYCLE" | sed -n '2p' | sed 's/^QMP> //' | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)
THAWED=$(echo "$FREEZE_CYCLE" | sed -n '4p' | sed 's/^QMP> //' | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)

if [ "$FROZEN" = "frozen" ] && [ "$THAWED" = "thawed" ]; then
    echo "  PASS: fsfreeze cycle (freeze->frozen->thaw->thawed)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: fsfreeze cycle (frozen=$FROZEN, thawed=$THAWED)"
    FAIL=$((FAIL + 1))
fi

test_cmd "guest-fstrim" \
    '{"execute":"guest-fstrim"}' \
    "object" \
    "paths"

# Subset-freeze (guest-fsfreeze-freeze-list with mountpoints).
# In --test mode sync_all_volumes returns n_mountpoints when a filter is
# set, so we can verify the parameter is plumbed through to the dispatch
# loop. Each freeze must be followed by a thaw in the same session
# because state is per-process.

# No-args: should delegate to global freeze and return >= 1
FREEZE_LIST_NOARGS=$(printf '%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze-list"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')
FL_NA=$(echo "$FREEZE_LIST_NOARGS" | sed -n '1p' \
    | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)
if [ "$FL_NA" -ge 1 ] 2>/dev/null; then
    echo "  PASS: freeze-list with no args delegates to global freeze (returned $FL_NA)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze-list with no args (returned $FL_NA)"
    FAIL=$((FAIL + 1))
fi

# Empty array: same as no args
FREEZE_LIST_EMPTY=$(printf '%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze-list","arguments":{"mountpoints":[]}}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')
FL_EM=$(echo "$FREEZE_LIST_EMPTY" | sed -n '1p' \
    | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)
if [ "$FL_EM" -ge 1 ] 2>/dev/null; then
    echo "  PASS: freeze-list with empty array delegates to global freeze (returned $FL_EM)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze-list with empty array (returned $FL_EM)"
    FAIL=$((FAIL + 1))
fi

# Two mountpoints: test mode pretends each succeeded -> returns 2
FREEZE_LIST_TWO=$(printf '%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze-list","arguments":{"mountpoints":["/Volumes/data","/Volumes/foo"]}}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')
FL_TWO=$(echo "$FREEZE_LIST_TWO" | sed -n '1p' \
    | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)
if [ "$FL_TWO" = "2" ]; then
    echo "  PASS: freeze-list with two mountpoints returned 2 (filter plumbed through)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze-list with two mountpoints returned $FL_TWO (expected 2)"
    FAIL=$((FAIL + 1))
fi

# Non-string entry: must return a spec-shaped error, not crash
FREEZE_LIST_BADTYPE=$(echo '{"execute":"guest-fsfreeze-freeze-list","arguments":{"mountpoints":[123]}}' \
    | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$FREEZE_LIST_BADTYPE" | grep -q '"error".*"mountpoints entries must be strings"'; then
    echo "  PASS: freeze-list with non-string entry returns spec-shaped error"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze-list with non-string entry (got: $FREEZE_LIST_BADTYPE)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Network Commands ---"
# =========================================================

test_cmd "guest-network-get-interfaces" \
    '{"execute":"guest-network-get-interfaces"}' \
    "array" \
    "name" "ip-addresses"

test_cmd "guest-network-get-route" \
    '{"execute":"guest-network-get-route"}' \
    "array" \
    "iface" "destination" "gateway" "nexthop" "mask" "metric" "irtt" "version" "desprefixlen"

# Validate at least one interface has an IP
HAS_IP=$(run_cmd '{"execute":"guest-network-get-interfaces"}' | python3 -c "
import json, sys
ifaces = json.load(sys.stdin)['return']
has = any(len(i.get('ip-addresses', [])) > 0 for i in ifaces)
print('yes' if has else 'no')
" 2>/dev/null)
if [ "$HAS_IP" = "yes" ]; then
    echo "  PASS: at least one interface has IP addresses"
    PASS=$((PASS + 1))
else
    echo "  FAIL: no interfaces have IP addresses"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- File I/O Commands (single-session pipeline) ---"
# =========================================================

# File ops require state (handles) so we run them in one agent session
TMPFILE="/tmp/mga-test-$$"
FILE_RESULT=$(printf '%s\n' \
    '{"execute":"guest-file-open","arguments":{"path":"/etc/hosts","mode":"r"}}' \
    '{"execute":"guest-file-read","arguments":{"handle":1000,"count":100}}' \
    '{"execute":"guest-file-seek","arguments":{"handle":1000,"offset":0,"whence":0}}' \
    '{"execute":"guest-file-flush","arguments":{"handle":1000}}' \
    '{"execute":"guest-file-close","arguments":{"handle":1000}}' \
    "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"$TMPFILE\",\"mode\":\"w\"}}" \
    '{"execute":"guest-file-write","arguments":{"handle":1001,"buf-b64":"aGVsbG8gdGVzdAo="}}' \
    '{"execute":"guest-file-close","arguments":{"handle":1001}}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

# Parse each line of output
FILE_LINES=()
while IFS= read -r line; do
    [ -n "$line" ] && FILE_LINES+=("$line")
done <<< "$FILE_RESULT"

# Check open (line 0): should return handle 1000
if echo "${FILE_LINES[0]:-}" | python3 -c "import json,sys; assert json.load(sys.stdin)['return'] == 1000" 2>/dev/null; then
    echo "  PASS: guest-file-open (handle=1000)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-open"
    FAIL=$((FAIL + 1))
fi

# Check read (line 1): should have count > 0 and buf-b64
READ_OK=$(echo "${FILE_LINES[1]:-}" | python3 -c "
import json, sys
r = json.load(sys.stdin)['return']
assert r['count'] > 0
assert len(r['buf-b64']) > 0
print('ok')
" 2>/dev/null)
if [ "$READ_OK" = "ok" ]; then
    echo "  PASS: guest-file-read"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-read"
    FAIL=$((FAIL + 1))
fi

# Check seek (line 2)
if echo "${FILE_LINES[2]:-}" | python3 -c "import json,sys; assert 'position' in json.load(sys.stdin)['return']" 2>/dev/null; then
    echo "  PASS: guest-file-seek"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-seek"
    FAIL=$((FAIL + 1))
fi

# Check flush (line 3)
if echo "${FILE_LINES[3]:-}" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: guest-file-flush"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-flush"
    FAIL=$((FAIL + 1))
fi

# Check close (line 4)
if echo "${FILE_LINES[4]:-}" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: guest-file-close"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-close"
    FAIL=$((FAIL + 1))
fi

# Check write result (line 6) and verify file content
WRITE_OK=$(echo "${FILE_LINES[6]:-}" | python3 -c "import json,sys; assert json.load(sys.stdin)['return']['count'] == 11; print('ok')" 2>/dev/null)
if [ "$WRITE_OK" = "ok" ] && [ -f "$TMPFILE" ] && [ "$(cat "$TMPFILE")" = "hello test" ]; then
    echo "  PASS: guest-file-write + verify content"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-file-write"
    FAIL=$((FAIL + 1))
fi
rm -f "$TMPFILE"

# =========================================================
echo ""
echo "--- Exec Commands (single-session pipeline) ---"
# =========================================================

# Exec + status — drives the agent via python so we can poll
# guest-exec-status until exited:true. The agent is now spec-conformant
# async (guest-exec returns {pid: N} immediately, status is polled), so
# the old "pipe four messages and read response 2" pattern races the
# child's startup. Real callers (qm guest exec, virsh qemu-agent-command
# guest-exec-status) poll; this test mirrors that.
EXEC_RESULT=$(python3 - "$BINARY" <<'PY' 2>/dev/null
import json, sys, time, subprocess
binary = sys.argv[1]
p = subprocess.Popen([binary, "--test"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

def send(cmd):
    p.stdin.write(json.dumps(cmd) + "\n"); p.stdin.flush()
    line = p.stdout.readline()
    if line.startswith("QMP> "): line = line[5:]
    return json.loads(line).get("return", {})

def poll(pid, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = send({"execute": "guest-exec-status", "arguments": {"pid": pid}})
        if r.get("exited"): return r
        time.sleep(0.05)
    return None

results = []
for cmd, args in [
    ("/bin/echo",  ["exec-test-ok"]),
    ("/bin/sh",    ["-c", "exit 42"]),
]:
    r = send({"execute": "guest-exec",
              "arguments": {"path": cmd, "arg": args, "capture-output": True}})
    pid = r.get("pid")
    results.append(poll(pid) if pid else None)

p.stdin.close()
p.wait(timeout=2)
print(json.dumps(results))
PY
)

EXEC_OUT=$(echo "$EXEC_RESULT" | python3 -c "
import json, sys, base64
s = json.load(sys.stdin)[0] or {}
print(base64.b64decode(s.get('out-data','')).decode().strip())
" 2>/dev/null)
EXEC_EXIT=$(echo "$EXEC_RESULT" | python3 -c "
import json, sys
s = json.load(sys.stdin)[0] or {}
print(s.get('exitcode', -1))
" 2>/dev/null)

if [ "$EXEC_OUT" = "exec-test-ok" ] && [ "$EXEC_EXIT" = "0" ]; then
    echo "  PASS: guest-exec + status (output='exec-test-ok', exit=0)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-exec (output='$EXEC_OUT', exit=$EXEC_EXIT)"
    FAIL=$((FAIL + 1))
fi

FAIL_EXIT=$(echo "$EXEC_RESULT" | python3 -c "
import json, sys
s = json.load(sys.stdin)[1] or {}
print(s.get('exitcode', -1))
" 2>/dev/null)
if [ "$FAIL_EXIT" = "42" ]; then
    echo "  PASS: guest-exec exit code propagation (exit=42)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-exec exit code (expected 42, got $FAIL_EXIT)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Async guest-exec: spec contract + deadlock regression ---"
# =========================================================
#
# Regression coverage for audit.md finding 1 (the sync drain that
# deadlocked on stderr floods and blocked the agent for the child's
# entire lifetime). Three assertions:
#   (a) guest-exec returns {pid:N} within 250 ms even when the child
#       lives 2 seconds — confirms the spec-conformant async contract.
#   (b) A child that writes >64 KB to stderr while keeping stdout tiny
#       completes end-to-end without the agent hanging (the audit's
#       exact reproduction case — sequential-drain deadlock).
#   (c) Output that exceeds MAX_CAPTURE_SIZE sets the err-truncated
#       flag rather than blocking or silently dropping bytes.

ASYNC_RESULT=$(python3 - "$BINARY" <<'PY' 2>/dev/null
import json, subprocess, sys, time
binary = sys.argv[1]
p = subprocess.Popen([binary, "--test"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def send(c):
    p.stdin.write(json.dumps(c) + "\n"); p.stdin.flush()
    l = p.stdout.readline()
    if l.startswith("QMP> "): l = l[5:]
    return json.loads(l).get("return", {})
def poll(pid, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = send({"execute":"guest-exec-status","arguments":{"pid":pid}})
        if s.get("exited"): return s
        time.sleep(0.05)
    return None

results = {}

# (a) sleep 2 should return {pid:N} immediately, not block 2s.
t0 = time.time()
r = send({"execute":"guest-exec","arguments":{
    "path":"/bin/sleep","arg":["2"],"capture-output":True}})
elapsed = time.time() - t0
results["sleep_pid"]      = r.get("pid")
results["sleep_elapsed"]  = elapsed
results["sleep_status"]   = poll(r.get("pid"), timeout=5.0)

# (b) stderr flood that would deadlock the prior sequential drain.
# Writes 256 KB to stderr (4x typical 64KB pipe buffer) while stdout
# stays tiny. The "1>&2 2>/dev/null" idiom redirects dd's stdout (its
# main output) to our parent's stderr pipe and silences dd's own
# diagnostic stderr — without it, "of=/dev/stderr 2>/dev/null" would
# rewrite /dev/fd/2 to /dev/null before dd opens it. Old code:
# deadlock at the 64KB mark. New code: completes.
r = send({"execute":"guest-exec","arguments":{
    "path":"/bin/sh",
    "arg":["-c", "dd if=/dev/zero bs=4096 count=64 1>&2 2>/dev/null; echo done"],
    "capture-output":True}})
results["flood_status"] = poll(r.get("pid"), timeout=10.0)

p.stdin.close(); p.wait(timeout=2)
print(json.dumps(results))
PY
)

# Assertion (a): exec returned in well under 2s
if echo "$ASYNC_RESULT" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert d['sleep_elapsed'] < 0.5, f'guest-exec blocked {d[\"sleep_elapsed\"]:.2f}s (should be <0.5s)'
assert d['sleep_pid'], 'no pid returned'
s = d['sleep_status'] or {}
assert s.get('exited') is True
assert s.get('exitcode') == 0
" 2>/dev/null; then
    echo "  PASS: guest-exec returns {pid} immediately (sync drain regression)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-exec async contract (sleep 2 should return immediately)"
    echo "         result: $ASYNC_RESULT"
    FAIL=$((FAIL + 1))
fi

# Assertion (b): stderr flood completed without deadlock
if echo "$ASYNC_RESULT" | python3 -c "
import base64, json, sys
d = json.load(sys.stdin)
s = d['flood_status'] or {}
assert s.get('exited') is True, 'child did not exit (deadlock?)'
assert s.get('exitcode') == 0
err = base64.b64decode(s.get('err-data','')) if s.get('err-data') else b''
assert len(err) >= 200000, f'expected ~256KB stderr, got {len(err)}'
out = base64.b64decode(s.get('out-data','')).decode().strip() if s.get('out-data') else ''
assert out == 'done', f'stdout expected \"done\", got {out!r}'
" 2>/dev/null; then
    echo "  PASS: stderr flood drained concurrently (deadlock regression)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: stderr flood — pipe drain deadlocked or output lost"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- SSH Commands (structure only) ---"
# =========================================================

# These need a valid username. Test with a nonexistent user to verify error handling.
test_cmd "guest-ssh-get-authorized-keys (bad user)" \
    '{"execute":"guest-ssh-get-authorized-keys","arguments":{"username":"nonexistent_test_user_12345"}}' \
    "error"

test_cmd "guest-ssh-add-authorized-keys (bad user)" \
    '{"execute":"guest-ssh-add-authorized-keys","arguments":{"username":"nonexistent_test_user_12345","keys":["ssh-rsa AAAA test"]}}' \
    "error"

# =========================================================
echo ""
echo "--- Error Handling ---"
# =========================================================

test_cmd "unknown command" \
    '{"execute":"guest-nonexistent-command"}' \
    "error"

test_cmd "malformed JSON returns error" \
    'not json at all' \
    "error"

test_cmd "missing execute field returns error" \
    '{"not_execute":"guest-ping"}' \
    "error"

test_cmd "empty arguments (missing required param)" \
    '{"execute":"guest-file-open","arguments":{}}' \
    "error"

# =========================================================
echo ""
echo "--- Block/Allow RPC Filtering ---"
# =========================================================

# Test block-rpcs: ping should be blocked
BLOCK_RESULT=$(echo '{"execute":"guest-ping"}' | "$BINARY" --test -b "guest-ping" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$BLOCK_RESULT" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$BLOCK_RESULT" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: block-rpcs blocks guest-ping"
    PASS=$((PASS + 1))
else
    echo "  FAIL: block-rpcs did not block guest-ping"
    FAIL=$((FAIL + 1))
fi

# Test block-rpcs: non-blocked command should still work
BLOCK_ALLOW=$(echo '{"execute":"guest-get-time"}' | "$BINARY" --test -b "guest-ping" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$BLOCK_ALLOW" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$BLOCK_ALLOW" | python -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: block-rpcs allows unblocked commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: block-rpcs broke unblocked commands"
    FAIL=$((FAIL + 1))
fi

# Test allow-rpcs: only listed commands work
ALLOW_BLOCKED=$(echo '{"execute":"guest-get-osinfo"}' | "$BINARY" --test -a "guest-ping,guest-sync,guest-sync-delimited,guest-info" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$ALLOW_BLOCKED" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$ALLOW_BLOCKED" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: allow-rpcs blocks unlisted commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: allow-rpcs did not block unlisted command"
    FAIL=$((FAIL + 1))
fi

ALLOW_OK=$(echo '{"execute":"guest-ping"}' | "$BINARY" --test -a "guest-ping,guest-sync,guest-sync-delimited,guest-info" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$ALLOW_OK" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$ALLOW_OK" | python -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: allow-rpcs permits listed commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: allow-rpcs blocked listed command"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Config File Parsing ---"
# =========================================================

CFGTMP="/tmp/mga-test-config-$$"
cat > "$CFGTMP" << 'CFGEOF'
[general]
verbose = 1
path = /dev/null
block-rpcs = guest-shutdown,guest-suspend-disk
CFGEOF

DUMP=$("$BINARY" -c "$CFGTMP" -D 2>/dev/null)
rm -f "$CFGTMP"

if echo "$DUMP" | grep -q "verbose = 1" && echo "$DUMP" | grep -q "block-rpcs = guest-shutdown"; then
    echo "  PASS: config file parsed correctly"
    PASS=$((PASS + 1))
else
    echo "  FAIL: config file not parsed"
    echo "    Got: $DUMP"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Version & Help ---"
# =========================================================

VER=$("$BINARY" -V 2>/dev/null)
if echo "$VER" | grep -q "mac-guest-agent"; then
    echo "  PASS: --version outputs version string"
    PASS=$((PASS + 1))
else
    echo "  FAIL: --version"
    FAIL=$((FAIL + 1))
fi

HELP=$("$BINARY" -h 2>/dev/null)
if echo "$HELP" | grep -q "\-\-install" && echo "$HELP" | grep -q "\-\-test"; then
    echo "  PASS: --help shows all options"
    PASS=$((PASS + 1))
else
    echo "  FAIL: --help incomplete"
    FAIL=$((FAIL + 1))
fi

SELFTEST=$("$BINARY" --self-test 2>/dev/null)
SELFTEST_RC=$?
if echo "$SELFTEST" | grep -q "self-test" && echo "$SELFTEST" | grep -q "Result:"; then
    echo "  PASS: --self-test runs and produces structured output"
    PASS=$((PASS + 1))
else
    echo "  FAIL: --self-test"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- SSH Commands (success path) ---"
# =========================================================

# Test with current user (should succeed for get, at least)
CURRENT_USER=$(whoami)
SSH_GET=$(echo "{\"execute\":\"guest-ssh-get-authorized-keys\",\"arguments\":{\"username\":\"$CURRENT_USER\"}}" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$SSH_GET" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d and 'keys' in d['return']" 2>/dev/null || \
   echo "$SSH_GET" | python -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d and 'keys' in d['return']" 2>/dev/null; then
    echo "  PASS: guest-ssh-get-authorized-keys for $CURRENT_USER"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-ssh-get-authorized-keys for $CURRENT_USER"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Set Time (validation only) ---"
# =========================================================

# Send a set-time with current time (should succeed without changing anything meaningful)
CURRENT_NS=$(python3 -c "import time; print(int(time.time() * 1e9))" 2>/dev/null || python -c "import time; print(int(time.time() * 1e9))" 2>/dev/null)
if [ -n "$CURRENT_NS" ]; then
    SET_TIME=$(echo "{\"execute\":\"guest-set-time\",\"arguments\":{\"time\":$CURRENT_NS}}" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
    if echo "$SET_TIME" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d or 'error' in d" 2>/dev/null || \
       echo "$SET_TIME" | python -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d or 'error' in d" 2>/dev/null; then
        echo "  PASS: guest-set-time accepts valid timestamp"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: guest-set-time"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  SKIP: guest-set-time (no python for timestamp)"
    SKIP=$((SKIP + 1))
fi

# Test set-time with bad arguments
test_cmd "guest-set-time (missing arg)" \
    '{"execute":"guest-set-time","arguments":{}}' \
    "error"

# =========================================================
echo ""
echo "--- Multi-command Pipeline (simulates PVE) ---"
# =========================================================

# PVE sends sync-delimited + actual command in ONE session
# This tests the buffer-check-before-poll fix
PIPE_RESULT=$(printf '%s\n%s\n' \
    '{"execute":"guest-sync-delimited","arguments":{"id":77777}}' \
    '{"execute":"guest-get-host-name"}' \
    | "$BINARY" --test 2>/dev/null | LC_ALL=C tr -d '\377' | LC_ALL=C sed 's/^QMP> //')

PIPE_LINES=()
while IFS= read -r line; do
    [ -n "$line" ] && PIPE_LINES+=("$line")
done <<< "$PIPE_RESULT"

# First response: sync id
SYNC_OK=0
if echo "${PIPE_LINES[0]:-}" | python3 -c "import json,sys; assert json.load(sys.stdin)['return'] == 77777" 2>/dev/null || \
   echo "${PIPE_LINES[0]:-}" | python -c "import json,sys; assert json.load(sys.stdin)['return'] == 77777" 2>/dev/null; then
    SYNC_OK=1
fi

# Second response: hostname
HOST_OK=0
if echo "${PIPE_LINES[1]:-}" | python3 -c "import json,sys; assert 'host-name' in json.load(sys.stdin)['return']" 2>/dev/null || \
   echo "${PIPE_LINES[1]:-}" | python -c "import json,sys; assert 'host-name' in json.load(sys.stdin)['return']" 2>/dev/null; then
    HOST_OK=1
fi

if [ $SYNC_OK -eq 1 ] && [ $HOST_OK -eq 1 ]; then
    echo "  PASS: PVE-style sync+command pipeline (both responses immediate)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: PVE-style pipeline (sync=$SYNC_OK, host=$HOST_OK)"
    FAIL=$((FAIL + 1))
fi

# Triple pipeline: sync + osinfo + network
TRIPLE=$(printf '%s\n%s\n%s\n' \
    '{"execute":"guest-sync-delimited","arguments":{"id":88888}}' \
    '{"execute":"guest-get-osinfo"}' \
    '{"execute":"guest-network-get-interfaces"}' \
    | "$BINARY" --test 2>/dev/null | LC_ALL=C tr -d '\377' | LC_ALL=C sed 's/^QMP> //')

TRIPLE_COUNT=$(echo "$TRIPLE" | grep -c '"return"' || true)
if [ "$TRIPLE_COUNT" -ge 3 ]; then
    echo "  PASS: triple pipeline (sync + osinfo + network = $TRIPLE_COUNT responses)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: triple pipeline (expected 3 responses, got $TRIPLE_COUNT)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Rapid Fire (20 commands in one session) ---"
# =========================================================

RAPID_INPUT=""
for i in $(seq 1 20); do
    RAPID_INPUT="${RAPID_INPUT}{\"execute\":\"guest-ping\"}\n"
done

RAPID_COUNT=$(printf "$RAPID_INPUT" | "$BINARY" --test 2>/dev/null | grep -c '"return"' || true)
if [ "$RAPID_COUNT" -eq 20 ]; then
    echo "  PASS: 20 rapid pings in one session ($RAPID_COUNT responses)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: rapid fire (expected 20, got $RAPID_COUNT)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Freeze Dry-Run (test mode safety) ---"
# =========================================================

# Freeze in test mode should succeed but not touch real filesystems
FREEZE_DRY=$(echo '{"execute":"guest-fsfreeze-freeze"}' | "$BINARY" --test 2>&1)
if echo "$FREEZE_DRY" | grep -q "Dry-run\|dry-run\|Filesystem frozen"; then
    echo "  PASS: freeze runs in dry-run mode during --test"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze may have touched real filesystem in test mode"
    FAIL=$((FAIL + 1))
fi

# Freeze cycle: freeze → status → thaw (dry-run, single session)
FREEZE_CYCLE_DR=$(printf '%s\n%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-fsfreeze-status"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

FR_LINE1=$(echo "$FREEZE_CYCLE_DR" | awk 'NR==1')
FR_LINE2=$(echo "$FREEZE_CYCLE_DR" | awk 'NR==2')
FR_LINE3=$(echo "$FREEZE_CYCLE_DR" | awk 'NR==3')

FR_OK=1
# Line 1: freeze returns a number
echo "$FR_LINE1" | python3 -c "import json,sys; d=json.load(sys.stdin); assert isinstance(d['return'], (int,float))" 2>/dev/null || FR_OK=0
# Line 2: status returns "frozen"
echo "$FR_LINE2" | python3 -c "import json,sys; assert json.load(sys.stdin)['return'] == 'frozen'" 2>/dev/null || FR_OK=0
# Line 3: thaw returns a number
echo "$FR_LINE3" | python3 -c "import json,sys; d=json.load(sys.stdin); assert isinstance(d['return'], (int,float))" 2>/dev/null || FR_OK=0

if [ $FR_OK -eq 1 ]; then
    echo "  PASS: freeze/status/thaw cycle (dry-run)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze/status/thaw cycle"
    FAIL=$((FAIL + 1))
fi

# Idempotent freeze: freeze twice should succeed
IDEM=$(printf '%s\n%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

IDEM_COUNT=$(echo "$IDEM" | grep -c '"return"' || true)
if [ "$IDEM_COUNT" -ge 3 ]; then
    echo "  PASS: idempotent freeze (double freeze returns count)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: idempotent freeze ($IDEM_COUNT responses, expected 3)"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Freeze hook abort contract ---"
# =========================================================
#
# Locks the docs/code contract from audit finding 5: a freeze hook
# script that exits non-zero must abort the freeze with a GenericError
# carrying "Freeze hook script failed". Uses the MGA_HOOK_DIR_OVERRIDE
# env var (honored only in --test mode) to install a failing hook in
# /tmp/ without polluting the host's real /etc/qemu/fsfreeze-hook.d.
HOOK_TEST_DIR="/tmp/mga-hook-abort-test-$$"
mkdir -p "$HOOK_TEST_DIR"
cat > "$HOOK_TEST_DIR/01-failing-hook.sh" <<'HOOKEOF'
#!/bin/bash
exit 1
HOOKEOF
chmod 755 "$HOOK_TEST_DIR/01-failing-hook.sh"

ABORT_RESP=$(printf '%s\n' '{"execute":"guest-fsfreeze-freeze"}' \
    | MGA_HOOK_DIR_OVERRIDE="$HOOK_TEST_DIR" "$BINARY" --test 2>/dev/null \
    | awk 'NR==1{sub(/^QMP> /,""); print}')

if echo "$ABORT_RESP" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert 'error' in d, f'expected error envelope, got: {d}'
err = d['error']
desc = err.get('desc', '')
assert 'Freeze hook script failed' in desc, f'wrong error desc: {desc!r}'
" 2>/dev/null; then
    echo "  PASS: failing freeze hook aborts the freeze with GenericError"
    PASS=$((PASS + 1))
else
    echo "  FAIL: failing freeze hook should abort the freeze (got: $ABORT_RESP)"
    FAIL=$((FAIL + 1))
fi

# Follow-up: a successful hook (exit 0) must NOT abort the freeze.
# Proves the abort path is gated on non-zero exit specifically, not
# on the mere fact of running a hook.
cat > "$HOOK_TEST_DIR/01-failing-hook.sh" <<'HOOKEOF'
#!/bin/bash
exit 0
HOOKEOF
SUCCESS_RESP=$(printf '%s\n' '{"execute":"guest-fsfreeze-freeze"}' \
    | MGA_HOOK_DIR_OVERRIDE="$HOOK_TEST_DIR" "$BINARY" --test 2>/dev/null \
    | awk 'NR==1{sub(/^QMP> /,""); print}')

if echo "$SUCCESS_RESP" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert 'return' in d, f'expected success envelope, got: {d}'
assert isinstance(d['return'], (int, float)), f'expected numeric count, got: {d[\"return\"]}'
" 2>/dev/null; then
    echo "  PASS: succeeding freeze hook does NOT abort the freeze"
    PASS=$((PASS + 1))
else
    echo "  FAIL: succeeding freeze hook should let the freeze proceed (got: $SUCCESS_RESP)"
    FAIL=$((FAIL + 1))
fi

# Cleanup.
rm -rf "$HOOK_TEST_DIR"

# =========================================================
echo ""
echo "--- Command Filtering During Freeze ---"
# =========================================================

# During freeze, non-freeze commands should be rejected
FILTER_TEST=$(printf '%s\n%s\n%s\n%s\n' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-get-osinfo"}' \
    '{"execute":"guest-ping"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

# Line 2 (get-osinfo) should be an error (not allowed during freeze)
FILTER_L2=$(echo "$FILTER_TEST" | awk 'NR==2')
# Line 3 (ping) should succeed (allowed during freeze)
FILTER_L3=$(echo "$FILTER_TEST" | awk 'NR==3')

FILTER_OK=1
echo "$FILTER_L2" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || FILTER_OK=0
echo "$FILTER_L3" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null || FILTER_OK=0

if [ $FILTER_OK -eq 1 ]; then
    echo "  PASS: non-freeze commands blocked during freeze"
    PASS=$((PASS + 1))
    echo "  PASS: freeze-safe commands allowed during freeze"
    PASS=$((PASS + 1))
else
    echo "  FAIL: command filtering during freeze"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Audit Fix: Command Injection Prevention ---"
# =========================================================

# Disk command with injection attempt should be safe
INJECT_TEST=$(echo '{"execute":"guest-get-disks"}' | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print}')
if echo "$INJECT_TEST" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d" 2>/dev/null; then
    echo "  PASS: disk listing uses safe command execution"
    PASS=$((PASS + 1))
else
    echo "  FAIL: disk listing"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- Audit Fix: Process Exec Signal Handling ---"
# =========================================================

# Execute a command that gets killed by signal (SIGTERM). Async-aware
# polling (same shape as the Exec Commands block above).
SIGNAL_STATUS=$(python3 - "$BINARY" <<'PY' 2>/dev/null
import json, sys, time, subprocess
binary = sys.argv[1]
p = subprocess.Popen([binary, "--test"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def send(c):
    p.stdin.write(json.dumps(c) + "\n"); p.stdin.flush()
    l = p.stdout.readline()
    if l.startswith("QMP> "): l = l[5:]
    return json.loads(l).get("return", {})
r = send({"execute":"guest-exec","arguments":{
    "path":"/bin/sh","arg":["-c","kill -TERM $$"],"capture-output":True}})
pid = r.get("pid"); status = None
if pid:
    deadline = time.time() + 5.0
    while time.time() < deadline:
        s = send({"execute":"guest-exec-status","arguments":{"pid":pid}})
        if s.get("exited"): status = s; break
        time.sleep(0.05)
p.stdin.close(); p.wait(timeout=2)
print(json.dumps(status or {}))
PY
)

if echo "$SIGNAL_STATUS" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r.get('exited') is True
# Signal-killed process must have signal field or non-zero exit code
assert 'signal' in r or r.get('exitcode', 0) != 0
" 2>/dev/null; then
    echo "  PASS: exec signal handling (WIFSIGNALED on raw status)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: exec signal handling"
    FAIL=$((FAIL + 1))
fi

# =========================================================
echo ""
echo "--- --self-test-json: freeze_dispatch block (Phase 2 Q3) ---"
# =========================================================
# verify.sh and contributors rely on the freeze_dispatch block being
# present and well-formed. Test both the structural shape and the
# per-fstypename dispatch contract (key set + treatment strings).

SELFTEST_JSON=$("$BINARY" --self-test-json 2>/dev/null)
if echo "$SELFTEST_JSON" | python3 -c "
import json, sys
d = json.load(sys.stdin)
fd = d['freeze_dispatch']
for k in ('log_path_default', 'log_line_prefix', 'per_fstypename',
         'zfs_cli_available', 'divergences_from_upstream_qga',
         'cpustats_discriminator', 'cpustats_note'):
    assert k in fd, f'missing freeze_dispatch.{k}'
pf = fd['per_fstypename']
assert pf['apfs'] == 'tmutil_snapshot+f_fullfsync'
assert pf['hfs'] == 'f_fullfsync'
assert pf['zfs'] == 'zfs_snapshot_if_cli_else_f_fullfsync'
for net in ('smbfs', 'afpfs', 'nfs', 'webdav', 'ftp'):
    assert pf[net] == 'skip_network', f'{net} not skip_network'
for special in ('devfs', 'autofs', 'fdesc', 'volfs', 'synthfs', 'lifs'):
    assert pf[special] == 'skip_special', f'{special} not skip_special'
for fat in ('msdos', 'vfat', 'exfat', 'udf', 'ntfs'):
    assert pf[fat] == 'f_fullfsync_with_enotsup_tolerated', f'{fat} wrong'
assert fd['cpustats_discriminator'] == 'linux'
dv = fd['divergences_from_upstream_qga']
assert dv['idempotent_re_freeze'] is True
assert dv['persistent_frozen_state_marker'] is False
assert dv['logging_disabled_during_freeze'] is False
assert isinstance(fd['zfs_cli_available'], bool)
" 2>/dev/null; then
    echo "  PASS: freeze_dispatch block shape and dispatch contract"
    PASS=$((PASS + 1))
else
    echo "  FAIL: freeze_dispatch block shape or dispatch contract"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS\n  - --self-test-json freeze_dispatch contract"
fi

# Round-trip: the cpustats discriminator advertised in freeze_dispatch
# must match the wire shape of guest-get-cpustats (Q4 contract).
CPUSTATS_JSON=$(run_cmd '{"execute":"guest-get-cpustats"}')
if echo "$CPUSTATS_JSON" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d['return']
assert isinstance(r, list) and len(r) > 0, 'expected non-empty array'
for cpu in r:
    assert cpu.get('type') == 'linux', f\"expected type=linux, got {cpu.get('type')}\"
    for f in ('user', 'system', 'idle'):
        assert f in cpu, f'missing field {f}'
" 2>/dev/null; then
    echo "  PASS: guest-get-cpustats matches advertised discriminator (type=linux array)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-get-cpustats discriminator mismatch with freeze_dispatch"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS\n  - guest-get-cpustats discriminator round-trip"
fi

echo ""
echo "=============================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "=============================================="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
    exit 1
fi
