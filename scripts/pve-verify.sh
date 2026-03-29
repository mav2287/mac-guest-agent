#!/bin/bash
# PVE Host-Side Verification Script
#
# Run from the Proxmox VE host to verify a macOS VM's guest agent.
#
# Usage: ./pve-verify.sh <vmid>
#
# Checks: config, ping, OS info, network, freeze round-trip, memory

set -e

VMID="${1:?Usage: $0 <vmid>}"
PASS=0
FAIL=0

check() {
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "PVE macOS Guest Agent Verification"
echo "==================================="
echo "VM ID: $VMID"
echo ""

# Config checks
echo "--- Configuration ---"
CONF="/etc/pve/qemu-server/$VMID.conf"
if [ -f "$CONF" ]; then
    if grep -q "agent:.*enabled=1.*type=isa" "$CONF"; then
        echo "  PASS  agent: enabled=1,type=isa"
        PASS=$((PASS + 1))
    elif grep -q "agent:.*enabled=1" "$CONF"; then
        echo "  WARN  agent enabled but type=isa not set (Apple's VirtIO agent may respond instead)"
        FAIL=$((FAIL + 1))
    else
        echo "  FAIL  agent not enabled in config"
        FAIL=$((FAIL + 1))
    fi

    if grep -q "discard=on" "$CONF"; then
        echo "  PASS  discard=on (TRIM enabled)"
        PASS=$((PASS + 1))
    else
        echo "  INFO  discard not enabled (optional for TRIM)"
    fi

    if grep -q "ssd=1" "$CONF"; then
        echo "  PASS  ssd=1 (SSD emulation)"
        PASS=$((PASS + 1))
    else
        echo "  INFO  ssd=1 not set (optional for TRIM)"
    fi
else
    echo "  FAIL  config file not found: $CONF"
    FAIL=$((FAIL + 1))
fi

# Agent communication
echo ""
echo "--- Agent Communication ---"
check "ping" qm agent "$VMID" ping

# OS info
OSINFO=$(qm agent "$VMID" get-osinfo 2>/dev/null)
if echo "$OSINFO" | grep -q "macOS\|Mac OS"; then
    echo "  PASS  get-osinfo ($(echo "$OSINFO" | grep -o '"pretty-name"[^,]*' | cut -d'"' -f4))"
    PASS=$((PASS + 1))
else
    echo "  FAIL  get-osinfo"
    FAIL=$((FAIL + 1))
fi

# Network
NETINFO=$(qm agent "$VMID" network-get-interfaces 2>/dev/null)
if echo "$NETINFO" | grep -q "ip-address"; then
    IP=$(echo "$NETINFO" | grep -o '"ip-address" *: *"[^"]*"' | head -1 | cut -d'"' -f4)
    echo "  PASS  network-get-interfaces (IP: ${IP:-unknown})"
    PASS=$((PASS + 1))
else
    echo "  FAIL  network-get-interfaces"
    FAIL=$((FAIL + 1))
fi

# Command count (via guest-info)
CMDCOUNT=$(qm agent "$VMID" info 2>/dev/null | grep -c '"name"' || echo "0")
if [ "$CMDCOUNT" -ge 40 ]; then
    echo "  PASS  command count: $CMDCOUNT (expected 45)"
    PASS=$((PASS + 1))
elif [ "$CMDCOUNT" -gt 0 ]; then
    echo "  WARN  command count: $CMDCOUNT (expected 45 — may be Apple's built-in agent)"
    FAIL=$((FAIL + 1))
else
    echo "  FAIL  could not get command count"
    FAIL=$((FAIL + 1))
fi

# Memory
echo ""
echo "--- Memory ---"
MEM=$(pvesh get "/nodes/$(hostname)/qemu/$VMID/status/current" 2>/dev/null)
MAXMEM=$(echo "$MEM" | grep maxmem | grep -o '[0-9]*' | head -1)
USEDMEM=$(echo "$MEM" | grep "│ mem " | grep -o '[0-9]*\.[0-9]*' | head -1)
if [ -n "$USEDMEM" ] && [ -n "$MAXMEM" ]; then
    echo "  PASS  memory reporting: ${USEDMEM}GB / $((MAXMEM / 1073741824))GB"
    PASS=$((PASS + 1))
else
    echo "  INFO  memory reporting: could not read (may need reboot)"
fi

# Freeze round-trip
echo ""
echo "--- Freeze/Thaw ---"
FREEZE=$(qm guest cmd "$VMID" fsfreeze-freeze 2>/dev/null)
if echo "$FREEZE" | grep -q "[0-9]"; then
    echo "  PASS  freeze"
    PASS=$((PASS + 1))

    STATUS=$(qm guest cmd "$VMID" fsfreeze-status 2>/dev/null)
    if echo "$STATUS" | grep -q "frozen"; then
        echo "  PASS  status: frozen"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  status not frozen after freeze"
        FAIL=$((FAIL + 1))
    fi

    THAW=$(qm guest cmd "$VMID" fsfreeze-thaw 2>/dev/null)
    if echo "$THAW" | grep -q "[0-9]"; then
        echo "  PASS  thaw"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  thaw failed"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL  freeze failed"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "==================================="
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "Status: ALL CHECKS PASSED"
else
    echo "Status: ISSUES FOUND"
fi
exit $FAIL
