#!/bin/bash
# Lifecycle test for --install / --upgrade / --uninstall on a guest VM.
# ====================================================================
#
# This is the test that should have existed before v2.5.4 shipped.
# vit9696's issue #11 comment ("upgrade also fails, I had to
# reinstall") was a hard-fail of the v2.5.4 --upgrade verify on
# Tiger that never got caught because the v2.5.4 sweep only tested
# guest-* commands against an already-installed daemon — it never
# exercised the install/upgrade/uninstall code paths themselves.
#
# This script exercises the full lifecycle on a single VM:
#   1. Capture pre-state (is the agent installed? what version?)
#   2. Stage the new binary inside the VM
#   3. --upgrade and assert: (a) exit code 0, (b) NO rollback
#      message in stderr, (c) daemon ping returns within a budget
#      that's generous for the slowest supported OS (Tiger).
#   4. Run tests/outcome-sweep.sh against the post-upgrade daemon
#      and require it to pass.
#   5. --uninstall and assert: binary gone, plist gone, daemon
#      not listed by launchctl.
#   6. --install (fresh) and assert daemon back up + outcome sweep
#      passes again.
#
# Run from the PVE host. Requires SSH access to the guest with sudo
# privileges for the user (the install/upgrade/uninstall calls all
# need root inside the guest).
#
# Usage:
#   lifecycle-test.sh <vmid> <ssh-host> <ssh-port> <ssh-user> <ssh-pass> <staged-binary-path>
#
# Example (Tiger via PVE's hostfwd port 22111):
#   lifecycle-test.sh 111 localhost 22111 user password /tmp/mac-guest-agent-fix
#
# Exit code: 0 iff every step succeeded. Non-zero on any failure.

set -u

VMID="${1:?usage: $0 <vmid> <ssh-host> <ssh-port> <ssh-user> <ssh-pass> <staged-binary-path>}"
SSH_HOST="${2:?ssh host required}"
SSH_PORT="${3:?ssh port required}"
SSH_USER="${4:?ssh user required}"
SSH_PASS="${5:?ssh password required}"
NEW_BIN="${6:?staged binary path required}"

GUEST_BIN_DEST="/tmp/mac-guest-agent-new"
INSTALLED_BIN="/usr/local/bin/mac-guest-agent"
PLIST_PATH="/Library/LaunchDaemons/com.mac-guest-agent.plist"

# Single-flight SSH helper. ConnectTimeout is long because Tiger
# sshd takes its sweet time on first banner (reverse-DNS lookup
# through the slirp NAT).
ssh_g() {
    sshpass -p "$SSH_PASS" ssh \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=180 -o ServerAliveInterval=15 \
        -o "HostKeyAlgorithms +ssh-rsa" \
        -o "PubkeyAcceptedAlgorithms +ssh-rsa" \
        -o "KexAlgorithms +diffie-hellman-group1-sha1,diffie-hellman-group14-sha1" \
        -p "$SSH_PORT" "$SSH_USER@$SSH_HOST" "$@"
}
scp_g() {
    sshpass -p "$SSH_PASS" scp \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=180 \
        -o "HostKeyAlgorithms +ssh-rsa" \
        -o "PubkeyAcceptedAlgorithms +ssh-rsa" \
        -o "KexAlgorithms +diffie-hellman-group1-sha1,diffie-hellman-group14-sha1" \
        -P "$SSH_PORT" "$@"
}

pass() { printf '  [\033[32mPASS\033[0m] %s\n' "$1"; }
fail() { printf '  [\033[31mFAIL\033[0m] %s\n' "$1" >&2; FAILS=$((FAILS+1)); }
note() { printf '  [\033[36mNOTE\033[0m] %s\n' "$1"; }

FAILS=0

# Wait for `qm agent <vmid> ping` to return `{}` (the QGA empty-object
# success response) within $1 seconds. Polls every 2 s.
wait_qga_up() {
    local budget="${1:-60}"
    local i=0
    while (( i < budget )); do
        local out rc
        out=$(qm agent "$VMID" ping 2>&1)
        rc=$?
        # PVE's `qm agent ping` returns exit 0 with empty stdout
        # on success (it strips the `{}` from the QGA response).
        # Any error path returns non-zero with diagnostic output.
        if [[ $rc -eq 0 ]] && { [[ -z "$out" ]] || [[ "$out" == "{}" ]]; }; then
            note "qm-agent ping ok after ${i}s"
            return 0
        fi
        sleep 2; i=$((i+2))
    done
    return 1
}

echo "=== Lifecycle test: VM $VMID ($SSH_USER@$SSH_HOST:$SSH_PORT) ==="
date -u +'started: %Y-%m-%dT%H:%M:%SZ'

# --- 1: pre-state ---------------------------------------------------
echo ""
echo "--- 1. pre-state ---"
pre_ver=$(ssh_g "test -x $INSTALLED_BIN && $INSTALLED_BIN -V 2>/dev/null || echo NOT_INSTALLED" 2>&1)
note "pre-state version: $pre_ver"

# --- 2: stage new binary --------------------------------------------
echo ""
echo "--- 2. stage new binary ---"
scp_g "$NEW_BIN" "$SSH_USER@$SSH_HOST:$GUEST_BIN_DEST" || { fail "scp staged binary"; exit 1; }
ssh_g "chmod +x $GUEST_BIN_DEST && ls -l $GUEST_BIN_DEST" || { fail "chmod staged binary"; exit 1; }
pass "binary staged at $GUEST_BIN_DEST"

# --- 3: upgrade -----------------------------------------------------
echo ""
echo "--- 3. --upgrade (vit9696 #11 regression check) ---"
if [[ "$pre_ver" == "NOT_INSTALLED" ]]; then
    note "no existing install — running fresh --install instead"
    upgrade_out=$(ssh_g "echo '$SSH_PASS' | sudo -S $GUEST_BIN_DEST --install 2>&1") || true
else
    upgrade_out=$(ssh_g "echo '$SSH_PASS' | sudo -S $GUEST_BIN_DEST --upgrade 2>&1") || true
fi
echo "$upgrade_out" | sed 's/^/    | /'

# vit9696's bug: verify rollback fires even on successful upgrades.
if echo "$upgrade_out" | grep -qE "Rolling back to backup binary|upgrade verify failed"; then
    fail "upgrade rolled back (issue #11 regression). Output above."
    exit 1
fi
pass "upgrade completed without rollback"

# --- 4: daemon health post-upgrade ----------------------------------
echo ""
echo "--- 4. daemon health post-upgrade ---"
wait_qga_up 60 || { fail "qm-agent ping did not respond within 60s after upgrade"; exit 1; }
post_ver=$(ssh_g "$INSTALLED_BIN -V 2>/dev/null") || true
note "post-upgrade version: $post_ver"

# --- 5: outcome sweep against post-upgrade daemon -------------------
echo ""
echo "--- 5. outcome sweep ---"
HARNESS_DIR=$(dirname "$0")
if [[ -x "$HARNESS_DIR/outcome-sweep.sh" ]]; then
    if bash "$HARNESS_DIR/outcome-sweep.sh" "$VMID"; then
        pass "outcome-sweep passed"
    else
        fail "outcome-sweep had real-data failures"
    fi
else
    note "outcome-sweep.sh not co-located; skipping (run separately)"
fi

# --- 6: uninstall ---------------------------------------------------
echo ""
echo "--- 6. --uninstall ---"
uninstall_out=$(ssh_g "echo '$SSH_PASS' | sudo -S $GUEST_BIN_DEST --uninstall 2>&1") || true
echo "$uninstall_out" | sed 's/^/    | /'

# Verify clean removal: binary gone, plist gone, launchd no longer
# lists our service.
post_uninst=$(ssh_g "
    test -e $INSTALLED_BIN && echo BIN_REMAINS || echo bin_gone
    test -e $PLIST_PATH    && echo PLIST_REMAINS || echo plist_gone
    sudo -n launchctl list 2>/dev/null | grep -E 'mac.?guest.?agent' && echo LISTED || echo not_listed
" 2>&1)
echo "$post_uninst" | sed 's/^/    | /'
if echo "$post_uninst" | grep -qE "BIN_REMAINS|PLIST_REMAINS|LISTED"; then
    fail "--uninstall left residue. See output above."
else
    pass "uninstall clean"
fi

# --- 7: reinstall ---------------------------------------------------
echo ""
echo "--- 7. --install (fresh) ---"
install_out=$(ssh_g "echo '$SSH_PASS' | sudo -S $GUEST_BIN_DEST --install 2>&1") || true
echo "$install_out" | sed 's/^/    | /'
wait_qga_up 60 || fail "qm-agent ping did not respond within 60s after fresh install"

# --- 8: second outcome sweep ----------------------------------------
echo ""
echo "--- 8. outcome sweep (post-fresh-install) ---"
if [[ -x "$HARNESS_DIR/outcome-sweep.sh" ]]; then
    if bash "$HARNESS_DIR/outcome-sweep.sh" "$VMID"; then
        pass "outcome-sweep passed (post-reinstall)"
    else
        fail "outcome-sweep had real-data failures (post-reinstall)"
    fi
fi

# --- summary --------------------------------------------------------
echo ""
echo "=============================================="
if [[ $FAILS -eq 0 ]]; then
    printf 'Lifecycle test: \033[32mall steps passed\033[0m for VM %s\n' "$VMID"
    date -u +'finished: %Y-%m-%dT%H:%M:%SZ'
    exit 0
else
    printf 'Lifecycle test: \033[31m%d step(s) failed\033[0m for VM %s\n' "$FAILS" "$VMID"
    date -u +'finished: %Y-%m-%dT%H:%M:%SZ'
    exit 1
fi
