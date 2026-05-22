#!/bin/bash
# PVE Host-Side Verification Script
#
# Run from the Proxmox VE host to verify a macOS VM's guest agent:
#   ./pve-verify.sh <vmid>
#
# Design notes:
#  - Every check that cannot *prove* success reports FAIL, never PASS. A
#    check that passes on data it could not parse is worse than no check —
#    it manufactures false confidence.
#  - Guest-agent responses are JSON; they are parsed with Perl JSON::PP
#    (a core module, always present on a Proxmox host), never scraped out
#    of formatted CLI text.
#  - All memory figures come from the guest agent itself. PVE's host-side
#    QMP/balloon memory counters are blank for macOS guests, because macOS
#    ships no virtio-balloon stats driver — so this script never reads them.
#
# 'set -e' is intentionally NOT used: a verification script must run every
# check, not abort on the first failure.

VMID="${1:?Usage: $0 <vmid>}"

PASS=0
FAIL=0

pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
info() { echo "  INFO  $1"; }

summary() {
    echo ""
    echo "==================================="
    echo "Results: $PASS passed, $FAIL failed"
    if [ "$FAIL" -eq 0 ]; then
        echo "Status: ALL CHECKS PASSED"
    else
        echo "Status: ISSUES FOUND"
    fi
}

# json_query <json-string> <perl-expression>
#
# Decodes <json-string> and evaluates <perl-expression> with the decoded
# data structure in $d, printing the result. Produces no output and exits
# non-zero if the input is not valid JSON — so a caller can tell an empty
# or malformed agent response apart from a genuine value.
json_query() {
    printf '%s' "$1" | perl -MJSON::PP -e '
        local $/;
        my $d = eval { decode_json(scalar <STDIN>) };
        exit 2 if $@ || !defined $d;
        my $out = eval $ARGV[0];
        exit 3 if $@;
        print $out if defined $out;
    ' -- "$2"
}

# --- Preflight -------------------------------------------------------------
for tool in qm perl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: '$tool' not found — run this script on the Proxmox VE host." >&2
        exit 2
    fi
done

echo "PVE macOS Guest Agent Verification"
echo "==================================="
echo "VM ID: $VMID"
echo ""

# --- Configuration ---------------------------------------------------------
echo "--- Configuration ---"
CONF="/etc/pve/qemu-server/$VMID.conf"
if [ -f "$CONF" ]; then
    # Match enabled=1 and type=isa independently — Proxmox may write the
    # agent options in either order ('enabled=1,type=isa' or the reverse).
    AGENT_LINE=$(grep -E '^agent:' "$CONF")
    if echo "$AGENT_LINE" | grep -qE '(^|[ ,])enabled=1([ ,]|$)'; then
        if echo "$AGENT_LINE" | grep -qE '(^|[ ,])type=isa([ ,]|$)'; then
            pass "agent: enabled=1, type=isa"
        else
            fail "agent enabled but type=isa not set — Apple's VirtIO agent may answer instead"
        fi
    else
        fail "guest agent not enabled in VM config"
    fi

    if grep -qE 'discard=on' "$CONF"; then
        pass "discard=on (TRIM enabled)"
    else
        info "discard not enabled (optional — needed for guest-fstrim)"
    fi

    if grep -qE 'ssd=1' "$CONF"; then
        pass "ssd=1 (SSD emulation)"
    else
        info "ssd=1 not set (optional — needed for guest-fstrim)"
    fi
else
    fail "VM config not found: $CONF"
fi

# --- VM state --------------------------------------------------------------
echo ""
echo "--- VM State ---"
VM_STATE=$(qm status "$VMID" 2>/dev/null | awk '{print $2}')
if [ "$VM_STATE" = "running" ]; then
    pass "VM $VMID is running"
else
    fail "VM $VMID is not running (state: ${VM_STATE:-unknown}) — cannot verify the guest agent"
    summary
    exit 1
fi

# --- Agent communication ---------------------------------------------------
echo ""
echo "--- Agent Communication ---"

if qm agent "$VMID" ping >/dev/null 2>&1; then
    pass "ping"
else
    fail "ping — agent not responding over the serial channel"
fi

OSINFO=$(qm agent "$VMID" get-osinfo 2>/dev/null)
PRETTY=$(json_query "$OSINFO" '$d->{"pretty-name"} // $d->{name}')
if [ -n "$PRETTY" ]; then
    pass "get-osinfo — $PRETTY"
else
    fail "get-osinfo — no valid response"
fi

NETINFO=$(qm agent "$VMID" network-get-interfaces 2>/dev/null)
IFCOUNT=$(json_query "$NETINFO" 'ref $d eq "ARRAY" ? scalar @$d : ""')
if [ -n "$IFCOUNT" ] && [ "$IFCOUNT" -gt 0 ] 2>/dev/null; then
    IP=$(json_query "$NETINFO" \
        'my $ip; for my $i (@$d) { for my $a (@{$i->{"ip-addresses"} || []}) { $ip ||= $a->{"ip-address"} } } $ip // "no IP"')
    pass "network-get-interfaces — $IFCOUNT interface(s), IP $IP"
else
    fail "network-get-interfaces — no valid response"
fi

AGENTINFO=$(qm agent "$VMID" info 2>/dev/null)
CMDCOUNT=$(json_query "$AGENTINFO" 'scalar @{$d->{supported_commands} || []}')
AGENTVER=$(json_query "$AGENTINFO" '$d->{version} // ""')
if [ -n "$CMDCOUNT" ] && [ "$CMDCOUNT" -ge 40 ] 2>/dev/null; then
    pass "info — $CMDCOUNT commands registered${AGENTVER:+, agent v$AGENTVER}"
elif [ -n "$CMDCOUNT" ] && [ "$CMDCOUNT" -gt 0 ] 2>/dev/null; then
    fail "info — only $CMDCOUNT commands (expected ~45; a different agent may be answering)"
else
    fail "info — no valid response from agent"
fi

# --- Memory (from the guest agent) -----------------------------------------
echo ""
echo "--- Memory (guest agent) ---"
# The agent encodes macOS memory as QGA memory blocks: total RAM is
# (block count x block size), used RAM is (online block count x block size).
# Block-quantised, hence the '~'. This is the agent's own data — PVE's
# host-side memory counters are blank for macOS guests.
BLKINFO=$(qm agent "$VMID" get-memory-block-info 2>/dev/null)
BLKSIZE=$(json_query "$BLKINFO" '$d->{size}')
BLOCKS=$(qm agent "$VMID" get-memory-blocks 2>/dev/null)
BLKCOUNTS=$(json_query "$BLOCKS" \
    'ref $d eq "ARRAY" ? scalar(@$d) . " " . scalar(grep { $_->{online} } @$d) : ""')

if [[ "$BLKSIZE" =~ ^[0-9]+$ ]] && [ "$BLKSIZE" -gt 0 ] && [ -n "$BLKCOUNTS" ]; then
    TOTAL_BLOCKS=${BLKCOUNTS%% *}
    ONLINE_BLOCKS=${BLKCOUNTS##* }
    if [[ "$TOTAL_BLOCKS" =~ ^[0-9]+$ ]] && [ "$TOTAL_BLOCKS" -gt 0 ]; then
        TOTAL_GB=$(awk "BEGIN { printf \"%.1f\", $TOTAL_BLOCKS * $BLKSIZE / 1073741824 }")
        USED_GB=$(awk "BEGIN { printf \"%.1f\", $ONLINE_BLOCKS * $BLKSIZE / 1073741824 }")
        pass "memory — agent reports ~${USED_GB} GB used / ~${TOTAL_GB} GB total ($ONLINE_BLOCKS/$TOTAL_BLOCKS blocks online)"
    else
        fail "memory — get-memory-blocks returned an empty block list"
    fi
else
    fail "memory — agent did not return valid memory data"
fi

# --- Freeze / thaw ---------------------------------------------------------
echo ""
echo "--- Freeze / Thaw ---"
# macOS has no FIFREEZE ioctl; the agent's freeze is sync + F_FULLFSYNC (HFS+)
# or a tmutil snapshot (APFS), plus an internal frozen flag. There is no kernel
# freeze-state to query, so beyond the command path this verifies the frozen
# STATE behaviourally: while frozen, the agent must REJECT non-freeze commands,
# and must resume normal operation after thaw. That is observable proof, not
# the agent's self-reported status string.
FREEZE=$(qm guest cmd "$VMID" fsfreeze-freeze 2>/dev/null)
FROZEN_N=$(echo "$FREEZE" | grep -oE '[0-9]+' | head -1)
if [ -n "$FROZEN_N" ] && [ "$FROZEN_N" -ge 1 ]; then
    pass "fsfreeze-freeze — $FROZEN_N filesystem(s) frozen"

    STATUS=$(qm guest cmd "$VMID" fsfreeze-status 2>/dev/null)
    if echo "$STATUS" | grep -qw "frozen"; then
        pass "fsfreeze-status — reports frozen"
    else
        fail "fsfreeze-status — not reported frozen after freeze"
    fi

    # Behavioural proof of the frozen state: a non-freeze command (get-osinfo)
    # must be rejected while frozen. If it answers, the agent is not genuinely
    # frozen — it only set its status flag.
    if qm agent "$VMID" get-osinfo >/dev/null 2>&1; then
        fail "frozen state — agent answered get-osinfo while frozen (not genuinely frozen)"
    else
        pass "frozen state — non-freeze command rejected while frozen"
    fi

    THAW=$(qm guest cmd "$VMID" fsfreeze-thaw 2>/dev/null)
    THAWED_N=$(echo "$THAW" | grep -oE '[0-9]+' | head -1)
    if [ -n "$THAWED_N" ] && [ "$THAWED_N" -ge 1 ]; then
        pass "fsfreeze-thaw — $THAWED_N filesystem(s) thawed"
    else
        fail "fsfreeze-thaw — thaw not confirmed; the VM filesystem may still be frozen"
    fi

    # Behavioural proof that the agent genuinely left the frozen state.
    if qm agent "$VMID" get-osinfo >/dev/null 2>&1; then
        pass "post-thaw — agent answers normally again"
    else
        fail "post-thaw — agent not responding after thaw"
    fi
elif [ -n "$FROZEN_N" ]; then
    fail "fsfreeze-freeze — froze 0 filesystems (freeze had no effect)"
else
    fail "fsfreeze-freeze — no response"
fi

summary
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
