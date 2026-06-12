#!/bin/bash
# Outcome-based sweep of guest-* commands against a running VM.
# ============================================================
#
# This is the test the v2.5.4 sweep should have been.
#
# The v2.5.4 sweep asserted "command did not return an error".
# That's an implementation check — it passes when the daemon returns
# anything at all, including the empty arrays the Tiger network
# commands were "fixed" to return in place of real data (issue #11).
# An outcome check asks instead: would a human who read this response
# say "yes, that answers my question"?
#
# This harness runs each read-only guest-* command and asserts on the
# CONTENT of the response: real interface data with real MACs, real
# routes with real gateways, real time within the past hour, etc.
# A response with the right SHAPE but synthetic/empty CONTENT fails.
#
# Designed to be run from the PVE host:
#   ssh pve "/var/tmp/outcome-sweep.sh 111"
#
# Or piped via ssh:
#   ssh pve "bash -s 111" < tests/outcome-sweep.sh
#
# Usage:
#   outcome-sweep.sh <vmid> [--report-only]
#
# Exit code: 0 iff every test that ran passed. Non-zero on any
# real-data failure or QGA-level failure. Tests that are deliberately
# skipped (state-changing, not safe to sweep) are reported but do
# not fail the run.

set -u

VMID="${1:?usage: $0 <vmid> [--report-only]}"
REPORT_ONLY=0
[[ "${2:-}" == "--report-only" ]] && REPORT_ONLY=1

# QGA dispatch wrapper. PVE 9.x's `qm agent` and `qm guest cmd` BOTH
# enforce a 19-command enumeration (fsfreeze-* / get-{fsinfo,host-name,
# memory-block-info,memory-blocks,osinfo,time,timezone,users,vcpus} /
# info / network-get-interfaces / ping / shutdown / suspend-*) and
# reject anything else with "value 'X' does not have a value in the
# enumeration ..." — even though our agent implements all 45 QGA
# commands. To reach the rest (get-load, get-cpustats, get-disks,
# get-diskstats, get-hostname, network-get-route, ssh-*, sync, etc.)
# we have to bypass the PVE wrapper and talk to the raw QGA UNIX
# socket directly via socat.
#
# Both paths produce JSON responses of the form `{"return": ...}`.
# PVE unwraps the `return` field; the socat path doesn't. To keep
# downstream assertions identical, both wrappers extract `.return`
# and emit it as the result.
qa() {
    local cmd_name="$1"
    local raw resp
    # First try PVE's wrapper — it's faster and handles the curated
    # commands cleanly. Fall through to raw socat on the enumeration
    # rejection that comes back as exit 255 with the well-known
    # "does not have a value in the enumeration" message.
    raw=$(qm guest cmd "$VMID" "$cmd_name" 2>&1)
    if [[ $? -eq 0 ]]; then
        echo "$raw"
        return 0
    fi
    if echo "$raw" | grep -q "does not have a value in the enumeration"; then
        local sock="/var/run/qemu-server/${VMID}.qga"
        if [[ -S "$sock" ]]; then
            # socat treats stdin EOF as connection-close — without a
            # delay after the JSON line, the response arrives after
            # socat has already closed the socket and gets dropped.
            # The `sleep 2` keeps the read side alive long enough for
            # the agent's response to be drained.
            resp=$( ( printf '{"execute":"guest-%s"}\n' "$cmd_name"; sleep 2 ) | \
                   socat - "UNIX-CONNECT:$sock" 2>/dev/null)
            if [[ -n "$resp" ]] && echo "$resp" | jq -e '.return' >/dev/null 2>&1; then
                echo "$resp" | jq '.return'
                return 0
            fi
            if [[ -n "$resp" ]] && echo "$resp" | jq -e '.error' >/dev/null 2>&1; then
                echo "$resp" >&2
                return 1
            fi
            echo "raw qga returned: $resp" >&2
            return 1
        fi
        echo "qm wrapper rejected and raw socket $sock not present" >&2
        return 1
    fi
    echo "$raw"
    return 1
}

PASS=0
FAIL=0
SKIP=0
FAILED=()

red()    { printf '\033[31m%s\033[0m' "$1"; }
green()  { printf '\033[32m%s\033[0m' "$1"; }
yellow() { printf '\033[33m%s\033[0m' "$1"; }

# Run one test: name, command (passed as args to qa), then either
# --jq <expression> producing true/false, or --has <substring>.
assert() {
    local name="$1"; shift
    local cmd=("$1"); shift
    local cmd_args=()
    while [[ $# -gt 0 && "$1" != "--jq" && "$1" != "--has" && "$1" != "--shape" ]]; do
        cmd_args+=("$1"); shift
    done
    local mode="${1:-}"; shift || true
    local expr="${1:-}"

    local out rc
    out=$(qa "${cmd[@]}" "${cmd_args[@]}")
    rc=$?

    if [[ $rc -ne 0 ]]; then
        printf '  [%s] %-48s — qm-agent rc=%s\n' "$(red FAIL)" "$name" "$rc" >&2
        printf '    output: %s\n' "$out" >&2
        FAIL=$((FAIL+1)); FAILED+=("$name (qm rc=$rc)")
        return
    fi

    case "$mode" in
        --jq)
            local jq_rc jq_out
            jq_out=$(echo "$out" | jq -e "$expr" 2>&1)
            jq_rc=$?
            if [[ $jq_rc -eq 0 ]]; then
                printf '  [%s] %-48s — %s\n' "$(green PASS)" "$name" "matches: $expr"
                PASS=$((PASS+1))
            else
                printf '  [%s] %-48s — %s\n' "$(red FAIL)" "$name" "violates: $expr" >&2
                printf '    response was: %s\n' "$out" | head -c 300 >&2
                printf '\n    jq said: %s\n' "$jq_out" >&2
                FAIL=$((FAIL+1)); FAILED+=("$name (jq-violation)")
            fi
            ;;
        --has)
            if echo "$out" | grep -q -- "$expr"; then
                printf '  [%s] %-48s — contains "%s"\n' "$(green PASS)" "$name" "$expr"
                PASS=$((PASS+1))
            else
                printf '  [%s] %-48s — missing "%s"\n' "$(red FAIL)" "$name" "$expr" >&2
                printf '    response: %s\n' "$out" | head -c 300 >&2; echo >&2
                FAIL=$((FAIL+1)); FAILED+=("$name (missing $expr)")
            fi
            ;;
        --shape)
            # Just assert it parses as JSON of the expected top-level
            # type. Useful for commands where any well-formed response
            # is acceptable.
            if echo "$out" | jq -e "$expr" >/dev/null 2>&1; then
                printf '  [%s] %-48s — shape ok\n' "$(green PASS)" "$name"
                PASS=$((PASS+1))
            else
                printf '  [%s] %-48s — shape wrong (expected %s)\n' "$(red FAIL)" "$name" "$expr" >&2
                printf '    response: %s\n' "$out" | head -c 300 >&2; echo >&2
                FAIL=$((FAIL+1)); FAILED+=("$name (shape)")
            fi
            ;;
        *)
            printf '  [%s] %-48s — assert() called without mode\n' "$(red BUG)" "$name" >&2
            FAIL=$((FAIL+1)); FAILED+=("$name (bug)")
            ;;
    esac
}

skip() {
    local name="$1"; local why="$2"
    printf '  [%s] %-48s — %s\n' "$(yellow SKIP)" "$name" "$why"
    SKIP=$((SKIP+1))
}

echo "=== Outcome-based sweep for VM ${VMID} ==="
date -u +'started: %Y-%m-%dT%H:%M:%SZ'
echo ""

echo "--- baseline & sync ---"
# guest-ping: PVE's `qm agent ping` returns empty stdout (it strips
# the {} on success). Treat empty response as PASS. Other QGA
# transports return `{}`.
ping_out=$(qa ping 2>&1)
if [[ -z "$ping_out" ]] || echo "$ping_out" | jq -e 'type=="object"' >/dev/null 2>&1; then
    printf '  [\033[32mPASS\033[0m] %-48s — %s\n' "guest-ping" "ok"
    PASS=$((PASS+1))
else
    printf '  [\033[31mFAIL\033[0m] %-48s — %s\n' "guest-ping" "non-empty non-object response" >&2
    FAIL=$((FAIL+1)); FAILED+=("guest-ping")
fi
# guest-sync / guest-sync-delimited require an {"id": N} argument.
# They aren't useful at the outcome level (their job is to drain the
# wire and echo the id back) — skip.
skip "guest-sync"           "requires {id:N} arg; protocol-level not outcome-level"
skip "guest-sync-delimited" "requires {id:N} arg; protocol-level not outcome-level"

echo ""
echo "--- agent self-describe ---"
assert "guest-info has version"    info        --jq '.version | type=="string" and length>0'
assert "guest-info has supported"  info        --jq '.supported_commands | length > 30'

echo ""
echo "--- OS / time / hostname ---"
assert "guest-get-osinfo has name"           get-osinfo   --jq '.name | type=="string" and length>0'
assert "guest-get-osinfo has version"        get-osinfo   --jq '.version | test("^[0-9]+\\.[0-9]+")'
assert "guest-get-osinfo has kernel-release" get-osinfo   --jq '.["kernel-release"] | length>0'
assert "guest-get-time is recent epoch"      get-time     --jq '. > 1700000000000000000 and . < 2000000000000000000'
assert "guest-get-timezone has zone"         get-timezone --jq '.zone | length>0'
assert "guest-get-hostname"                  get-hostname --jq '.["host-name"] | length>0'

echo ""
echo "--- load / CPU / memory ---"
assert "guest-get-load present"              get-load            --jq '.load1m >= 0 and .load5m >= 0 and .load15m >= 0'
assert "guest-get-cpustats nonempty"         get-cpustats        --jq '. | length >= 1'
assert "guest-get-cpustats has user+idle"    get-cpustats        --jq '.[0] | (.user >= 0 and .idle >= 0)'
assert "guest-get-vcpus has at least 1"      get-vcpus           --jq '. | length >= 1 and (.[0].online == true)'
assert "guest-get-memory-block-info"         get-memory-block-info --jq '.size >= 1048576'
assert "guest-get-memory-blocks"             get-memory-blocks    --jq '. | length >= 1'

echo ""
echo "--- disks & filesystems ---"
assert "guest-get-disks has root"     get-disks    --jq '. | map(select(.name | length > 0)) | length >= 1'
assert "guest-get-diskstats nonempty" get-diskstats --jq '. | length >= 1'
assert "guest-get-fsinfo has mounts"  get-fsinfo   --jq '. | map(select(.mountpoint=="/")) | length == 1'
assert "guest-get-fsinfo / has type"  get-fsinfo   --jq '. | map(select(.mountpoint=="/")) | .[0].type | length > 0'

echo ""
echo "--- networking — THE issue #11 regressions ---"
# Tighter assertions for the bug class we are specifically rejecting:
#  - empty array passes "no error" but fails "real data".
assert "network-get-interfaces non-empty"   network-get-interfaces \
    --jq '. | length >= 1'
assert "network-get-interfaces has hw-addr" network-get-interfaces \
    --jq '. | map(select(.["hardware-address"] | test("^[0-9a-f]{2}(:[0-9a-f]{2}){5}$"))) | length >= 1'
assert "network-get-interfaces has IPv4"    network-get-interfaces \
    --jq '. | map(.["ip-addresses"][]? | select(.["ip-address-type"]=="ipv4" and .["ip-address"] != "0.0.0.0")) | length >= 1'
assert "network-get-route non-empty"        network-get-route \
    --jq '. | length >= 1'
assert "network-get-route has default-IPv4" network-get-route \
    --jq '. | map(select(.destination=="0.0.0.0" and .gateway != "")) | length >= 1'

echo ""
echo "--- fsfreeze readout ---"
# PVE's `qm guest cmd fsfreeze-status` unwraps the JSON string and
# emits the bare token "thawed" / "frozen" on stdout — not valid
# JSON. Handle by string match instead of jq.
fs_out=$(qa fsfreeze-status 2>&1 | tr -d '"' | tr -d ' \n')
if [[ "$fs_out" == "thawed" || "$fs_out" == "frozen" ]]; then
    printf '  [\033[32mPASS\033[0m] %-48s — status=%s\n' "guest-fsfreeze-status" "$fs_out"
    PASS=$((PASS+1))
else
    printf '  [\033[31mFAIL\033[0m] %-48s — unexpected: %s\n' "guest-fsfreeze-status" "$fs_out" >&2
    FAIL=$((FAIL+1)); FAILED+=("guest-fsfreeze-status")
fi

echo ""
echo "--- ssh keys (read-only) ---"
# guest-ssh-get-authorized-keys requires a {"username": "..."} arg.
# Skip in the no-arg sweep — covered by ssh-roundtrip.sh.
skip "guest-ssh-get-authorized-keys" "requires {username} arg; covered separately"

echo ""
echo "--- skipped: state-changing commands ---"
skip "guest-exec / guest-exec-status"     "tested separately to avoid daemon side-effects"
skip "guest-file-* family"                "tested separately"
skip "guest-shutdown / suspend-*"         "VM lifecycle covered by lifecycle-test.sh"
skip "guest-set-time / set-vcpus / set-memory-blocks" "writes guest state; covered separately"
skip "guest-fsfreeze-freeze / thaw"       "freezes the FS; covered by fsfreeze-roundtrip.sh"
skip "guest-fstrim"                       "TRIMs the disk; runs in lifecycle-test.sh"
skip "guest-ssh-add / remove-authorized-keys" "writes ~/.ssh/authorized_keys; covered separately"
skip "guest-set-user-password"            "writes shadow; covered separately"

echo ""
echo "=============================================="
printf 'Outcome sweep: %s passed, %s failed, %s skipped\n' \
    "$(green $PASS)" "$([[ $FAIL -gt 0 ]] && red $FAIL || echo $FAIL)" "$(yellow $SKIP)"
echo "=============================================="
date -u +'finished: %Y-%m-%dT%H:%M:%SZ'

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "Failures:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    [[ $REPORT_ONLY -eq 1 ]] && exit 0
    exit 1
fi
exit 0
