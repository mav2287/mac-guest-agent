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

ARCH=$(file "$BINARY" | grep -o 'arm64\|x86_64\|i386' | head -1)
echo "=============================================="
echo " macOS Guest Agent Test Suite"
echo " Binary: $BINARY ($ARCH)"
echo " Host:   $(sw_vers -productName 2>/dev/null || echo 'unknown') $(sw_vers -productVersion 2>/dev/null || echo '')"
echo "=============================================="
echo ""

# Run a command and capture the JSON response (strip the "QMP> " prompt)
run_cmd() {
    local input="$1"
    echo "$input" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}'
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
    "load1" "load5" "load15"

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
    "object" \
    "user" "system" "idle" "nice"

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
    "object"

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

# =========================================================
echo ""
echo "--- Network Commands ---"
# =========================================================

test_cmd "guest-network-get-interfaces" \
    '{"execute":"guest-network-get-interfaces"}' \
    "array" \
    "name" "ip-addresses"

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

# Exec + status in one session
EXEC_RESULT=$(printf '%s\n' \
    '{"execute":"guest-exec","arguments":{"path":"/bin/echo","arg":["exec-test-ok"],"capture-output":true}}' \
    '{"execute":"guest-exec-status","arguments":{"pid":1}}' \
    '{"execute":"guest-exec","arguments":{"path":"/bin/sh","arg":["-c","exit 42"],"capture-output":true}}' \
    '{"execute":"guest-exec-status","arguments":{"pid":2}}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

EXEC_LINES=()
while IFS= read -r line; do
    [ -n "$line" ] && EXEC_LINES+=("$line")
done <<< "$EXEC_RESULT"

# Check exec output (line 1 = status of pid 1)
EXEC_OUT=$(echo "${EXEC_LINES[1]:-}" | python3 -c "
import json, sys, base64
r = json.load(sys.stdin)['return']
print(base64.b64decode(r.get('out-data','')).decode().strip())
" 2>/dev/null)
EXEC_EXIT=$(echo "${EXEC_LINES[1]:-}" | python3 -c "import json,sys; print(json.load(sys.stdin)['return'].get('exitcode',-1))" 2>/dev/null)

if [ "$EXEC_OUT" = "exec-test-ok" ] && [ "$EXEC_EXIT" = "0" ]; then
    echo "  PASS: guest-exec + status (output='exec-test-ok', exit=0)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-exec (output='$EXEC_OUT', exit=$EXEC_EXIT)"
    FAIL=$((FAIL + 1))
fi

# Check failing command (line 3 = status of pid 2)
FAIL_EXIT=$(echo "${EXEC_LINES[3]:-}" | python3 -c "import json,sys; print(json.load(sys.stdin)['return'].get('exitcode',-1))" 2>/dev/null)
if [ "$FAIL_EXIT" = "42" ]; then
    echo "  PASS: guest-exec exit code propagation (exit=42)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: guest-exec exit code (expected 42, got $FAIL_EXIT)"
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

# malformed JSON and missing execute field are silently discarded
# (no response sent — prevents stale data in serial buffer)
echo "  SKIP: malformed JSON (silently discarded, no response)"
echo "  SKIP: missing execute field (silently discarded, no response)"
SKIP=$((SKIP + 2))

test_cmd "empty arguments (missing required param)" \
    '{"execute":"guest-file-open","arguments":{}}' \
    "error"

# =========================================================
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
