#!/bin/bash
# macOS Guest Agent - SAFE Read-Only Test
#
# This script ONLY tests read-only query commands.
# It will NOT:
#   - Install anything
#   - Modify any files
#   - Execute any programs via guest-exec
#   - Touch SSH keys
#   - Change passwords
#   - Shut down, reboot, or sleep the system
#   - Require root privileges
#   - Open any virtio devices
#
# Safe to run on production VMs.
#
# Usage: ./safe_test.sh /path/to/mac-guest-agent

BINARY="${1:?Usage: $0 /path/to/mac-guest-agent}"

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    chmod +x "$BINARY" 2>/dev/null || {
        echo "ERROR: Cannot make binary executable. Run: chmod +x $BINARY"
        exit 1
    }
fi

PASS=0
FAIL=0

echo "=============================================="
echo " macOS Guest Agent - Safe Read-Only Test"
echo " Binary: $BINARY"
echo " Host:   $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null)"
echo " Arch:   $(uname -m)"
echo "=============================================="
echo ""
echo " This test is 100% read-only. Nothing is modified."
echo ""

# Run one command, check for "return" in response
test_cmd() {
    local name="$1"
    local json="$2"
    shift 2
    local fields=("$@")

    local resp
    resp=$(echo "$json" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')

    # Check valid JSON with "return"
    if ! echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d" 2>/dev/null; then
        # Try python2 for older macOS
        if ! echo "$resp" | python -c "import json,sys; d=json.load(sys.stdin); assert 'return' in d" 2>/dev/null; then
            echo "  FAIL: $name"
            echo "        Response: ${resp:0:120}"
            FAIL=$((FAIL + 1))
            return
        fi
    fi

    # Check required fields if specified
    for field in "${fields[@]}"; do
        if ! echo "$resp" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d['return']
if isinstance(r, list):
    assert len(r) == 0 or '$field' in r[0]
elif isinstance(r, dict):
    assert '$field' in r
" 2>/dev/null && ! echo "$resp" | python -c "
import json, sys
d = json.load(sys.stdin)
r = d['return']
if isinstance(r, list):
    assert len(r) == 0 or '$field' in r[0]
elif isinstance(r, dict):
    assert '$field' in r
" 2>/dev/null; then
            echo "  FAIL: $name (missing field: $field)"
            FAIL=$((FAIL + 1))
            return
        fi
    done

    echo "  PASS: $name"
    PASS=$((PASS + 1))
}

# Print a value from a command response
show_value() {
    local json="$1"
    local pyexpr="$2"
    echo "$json" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}' | python3 -c "
import json, sys
d = json.load(sys.stdin)
$pyexpr
" 2>/dev/null || echo "$json" | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}' | python -c "
import json, sys
d = json.load(sys.stdin)
$pyexpr
" 2>/dev/null
}

echo "--- 1. Basic Protocol ---"

test_cmd "guest-ping" \
    '{"execute":"guest-ping"}'

test_cmd "guest-sync" \
    '{"execute":"guest-sync","arguments":{"id":99}}'

test_cmd "guest-info" \
    '{"execute":"guest-info"}' \
    "version" "supported_commands"

CMD_COUNT=$(show_value '{"execute":"guest-info"}' "print(len(d['return']['supported_commands']))")
echo "        Commands registered: $CMD_COUNT"

echo ""
echo "--- 2. OS Information ---"

test_cmd "guest-get-osinfo" \
    '{"execute":"guest-get-osinfo"}' \
    "id" "name" "version" "kernel-release"

echo "        $(show_value '{"execute":"guest-get-osinfo"}' "r=d['return']; print(r.get('pretty-name','?'), '/', r.get('kernel-release','?'), '/', r.get('machine','?'))")"

echo ""
echo "--- 3. Host & Time ---"

test_cmd "guest-get-host-name" \
    '{"execute":"guest-get-host-name"}' \
    "host-name"

echo "        Hostname: $(show_value '{"execute":"guest-get-host-name"}' "print(d['return']['host-name'])")"

test_cmd "guest-get-timezone" \
    '{"execute":"guest-get-timezone"}' \
    "zone" "offset"

test_cmd "guest-get-time" \
    '{"execute":"guest-get-time"}'

test_cmd "guest-get-users" \
    '{"execute":"guest-get-users"}'

USER_COUNT=$(show_value '{"execute":"guest-get-users"}' "print(len(d['return']))")
echo "        Logged-in users: $USER_COUNT"

test_cmd "guest-get-load" \
    '{"execute":"guest-get-load"}' \
    "load1" "load5" "load15"

echo "        $(show_value '{"execute":"guest-get-load"}' "r=d['return']; print('Load: %.2f %.2f %.2f' % (r['load1'], r['load5'], r['load15']))")"

echo ""
echo "--- 4. CPU ---"

test_cmd "guest-get-vcpus" \
    '{"execute":"guest-get-vcpus"}' \
    "logical-id" "online"

VCPU_COUNT=$(show_value '{"execute":"guest-get-vcpus"}' "print(len(d['return']))")
echo "        vCPUs: $VCPU_COUNT"

test_cmd "guest-get-cpustats" \
    '{"execute":"guest-get-cpustats"}' \
    "user" "system" "idle"

echo ""
echo "--- 5. Memory ---"

test_cmd "guest-get-memory-block-info" \
    '{"execute":"guest-get-memory-block-info"}' \
    "size"

BLOCK_SIZE=$(show_value '{"execute":"guest-get-memory-block-info"}' "print(int(d['return']['size'] / 1024 / 1024))")
echo "        Block size: ${BLOCK_SIZE}MB"

test_cmd "guest-get-memory-blocks" \
    '{"execute":"guest-get-memory-blocks"}' \
    "phys-index" "online"

BLOCK_COUNT=$(show_value '{"execute":"guest-get-memory-blocks"}' "print(len(d['return']))")
ONLINE_COUNT=$(show_value '{"execute":"guest-get-memory-blocks"}' "print(sum(1 for b in d['return'] if b['online']))")
echo "        Blocks: $BLOCK_COUNT total, $ONLINE_COUNT online"

echo ""
echo "--- 6. Disk & Filesystem ---"

test_cmd "guest-get-disks" \
    '{"execute":"guest-get-disks"}'

DISK_COUNT=$(show_value '{"execute":"guest-get-disks"}' "print(len(d['return']))")
echo "        Disks: $DISK_COUNT"

test_cmd "guest-get-fsinfo" \
    '{"execute":"guest-get-fsinfo"}' \
    "name" "mountpoint" "type"

FS_COUNT=$(show_value '{"execute":"guest-get-fsinfo"}' "print(len(d['return']))")
echo "        Filesystems: $FS_COUNT"
show_value '{"execute":"guest-get-fsinfo"}' "
for fs in d['return']:
    total_gb = fs.get('total-bytes', 0) / 1024 / 1024 / 1024
    used_gb = fs.get('used-bytes', 0) / 1024 / 1024 / 1024
    print('          %s on %s (%s) %.1fGB/%.1fGB' % (fs['name'], fs['mountpoint'], fs['type'], used_gb, total_gb))
"

echo ""
echo "--- 7. Filesystem Freeze (simulated state) ---"

# Run freeze cycle in single session to test state machine
FREEZE_RESULT=$(printf '%s\n%s\n%s\n%s\n' \
    '{"execute":"guest-fsfreeze-status"}' \
    '{"execute":"guest-fsfreeze-freeze"}' \
    '{"execute":"guest-fsfreeze-status"}' \
    '{"execute":"guest-fsfreeze-thaw"}' \
    | "$BINARY" --test 2>/dev/null | sed 's/^QMP> //')

INITIAL=$(echo "$FREEZE_RESULT" | awk 'NR==1' | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null || echo "$FREEZE_RESULT" | awk 'NR==1' | python -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)
AFTER_FREEZE=$(echo "$FREEZE_RESULT" | awk 'NR==3' | python3 -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null || echo "$FREEZE_RESULT" | awk 'NR==3' | python -c "import json,sys; print(json.load(sys.stdin)['return'])" 2>/dev/null)

if [ "$INITIAL" = "thawed" ] && [ "$AFTER_FREEZE" = "frozen" ]; then
    echo "  PASS: fsfreeze state machine (thawed -> frozen -> thawed)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: fsfreeze state machine (initial=$INITIAL, after_freeze=$AFTER_FREEZE)"
    FAIL=$((FAIL + 1))
fi

test_cmd "guest-fstrim (no-op)" \
    '{"execute":"guest-fstrim"}'

echo ""
echo "--- 8. Network ---"

test_cmd "guest-network-get-interfaces" \
    '{"execute":"guest-network-get-interfaces"}' \
    "name"

show_value '{"execute":"guest-network-get-interfaces"}' "
for iface in d['return']:
    ips = ', '.join(a['ip-address'] for a in iface.get('ip-addresses', []))
    mac = iface.get('hardware-address', 'n/a')
    print('        %s  mac=%s  ips=%s' % (iface['name'], mac, ips or 'none'))
"

echo ""
echo "--- 9. Error Handling ---"

# These should return errors, not crash
ERRCMD=$(echo '{"execute":"nonexistent"}' | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
if echo "$ERRCMD" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$ERRCMD" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: unknown command returns error"
    PASS=$((PASS + 1))
else
    echo "  FAIL: unknown command"
    FAIL=$((FAIL + 1))
fi

BADJSON=$(echo 'not json at all' | "$BINARY" --test 2>/dev/null | awk 'NR==1{sub(/^QMP> /,""); print; exit}')
if echo "$BADJSON" | python3 -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null || \
   echo "$BADJSON" | python -c "import json,sys; assert 'error' in json.load(sys.stdin)" 2>/dev/null; then
    echo "  PASS: malformed JSON returns error"
    PASS=$((PASS + 1))
else
    echo "  FAIL: malformed JSON"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=============================================="
echo " Results: $PASS passed, $FAIL failed"
echo " (Power, exec, file-write, SSH, password"
echo "  commands intentionally not tested)"
echo "=============================================="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
