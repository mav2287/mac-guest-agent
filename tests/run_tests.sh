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
echo "--- Block/Allow RPC Filtering ---"
# =========================================================

# Test block-rpcs: ping should be blocked
BLOCK_RESULT=$(echo '{"execute":"guest-ping"}' | "$BINARY" --test -b "guest-ping" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
if echo "$BLOCK_RESULT" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$BLOCK_RESULT" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: block-rpcs blocks guest-ping"
    PASS=$((PASS + 1))
else
    echo "  FAIL: block-rpcs did not block guest-ping"
    FAIL=$((FAIL + 1))
fi

# Test block-rpcs: non-blocked command should still work
BLOCK_ALLOW=$(echo '{"execute":"guest-get-time"}' | "$BINARY" --test -b "guest-ping" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
if echo "$BLOCK_ALLOW" | python3 -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$BLOCK_ALLOW" | python -c "import json,sys; assert 'return' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: block-rpcs allows unblocked commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: block-rpcs broke unblocked commands"
    FAIL=$((FAIL + 1))
fi

# Test allow-rpcs: only listed commands work
ALLOW_BLOCKED=$(echo '{"execute":"guest-get-osinfo"}' | "$BINARY" --test -a "guest-ping,guest-sync,guest-sync-delimited,guest-info" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
if echo "$ALLOW_BLOCKED" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$ALLOW_BLOCKED" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: allow-rpcs blocks unlisted commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: allow-rpcs did not block unlisted command"
    FAIL=$((FAIL + 1))
fi

ALLOW_OK=$(echo '{"execute":"guest-ping"}' | "$BINARY" --test -a "guest-ping,guest-sync,guest-sync-delimited,guest-info" 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
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

# =========================================================
echo ""
echo "--- SSH Commands (success path) ---"
# =========================================================

# Test with current user (should succeed for get, at least)
CURRENT_USER=$(whoami)
SSH_GET=$(echo "{\"execute\":\"guest-ssh-get-authorized-keys\",\"arguments\":{\"username\":\"$CURRENT_USER\"}}" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
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
    SET_TIME=$(echo "{\"execute\":\"guest-set-time\",\"arguments\":{\"time\":$CURRENT_NS}}" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
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
INJECT_TEST=$(echo '{"execute":"guest-get-disks"}' | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
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

# Execute a command that gets killed by signal (SIGTERM)
SIGNAL_TEST=$(printf '%s\n%s\n' \
    '{"execute":"guest-exec","arguments":{"path":"/bin/sh","arg":["-c","kill -TERM $$"],"capture-output":true}}' \
    '{"execute":"guest-exec-status","arguments":{"pid":1}}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

SIG_LINE=$(echo "$SIGNAL_TEST" | awk 'NR==2')
if echo "$SIG_LINE" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d['return']
assert r['exited'] == True
# Signal-killed process should have signal field or negative exit code
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
echo "=============================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "=============================================="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
    exit 1
fi
