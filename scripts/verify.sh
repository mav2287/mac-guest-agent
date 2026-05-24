#!/bin/bash
# mac-guest-agent — host-side verification (unified, multi-transport)
#
#   ./verify.sh [--transport pve|libvirt|utm|qga-socket] <identifier> [options]
#
# A single host-side invocation produces the full Tier-2 → Tier-1 evidence
# for a macOS guest running mac-guest-agent: configuration check, VM-state
# check, host-driven QGA round-trip (ping/get-osinfo/network/info/memory),
# freeze/thaw round-trip with a content-based behavioural check (not exit
# code — see docs/research/UPSTREAM_NOTES.md Target 4 for why), and in-VM
# `mac-guest-agent --self-test-json` + `--safe-test-json` driven over the
# transport's guest-exec channel.
#
# Transport plugins:
#   pve         — qm agent + qm guest exec
#   libvirt     — virsh qemu-agent-command (added in commit 2)
#   utm         — Unix-socket I/O against UTM's QGA serial (commit 3)
#   qga-socket  — Unix-socket I/O against a path supplied via --qga-socket
#
# Identifier semantics per transport:
#   pve         — numeric VMID
#   libvirt     — domain name
#   utm         — VM name or UUID
#   qga-socket  — purely cosmetic; --qga-socket PATH is what gets used
#
# Auto-detection (when --transport is omitted):
#   identifier is all digits AND `qm` on PATH AND
#     /etc/pve/qemu-server/<id>.conf exists           → pve
#   `virsh` on PATH AND `virsh dominfo <id>` exits 0  → libvirt
#   `utmctl` on PATH AND `utmctl status <id>` matches → utm
#   else                                               → error
#
# Options:
#   --transport NAME    Force a transport (skip auto-detect).
#   --qga-socket PATH   Direct path to a QGA Unix socket. Required for
#                       transport=qga-socket; optional override for utm.
#   --agent-path PATH   Guest path to mac-guest-agent binary
#                       (default: /usr/local/bin/mac-guest-agent).
#   --log-path PATH     Guest path to the agent log
#                       (default: /var/log/mac-guest-agent.log).
#   --exec-timeout SEC  guest-exec timeout per in-VM command (default: 30).
#   --no-redact         Disable PII redaction (IPs, MAC addresses, VM ID).
#                       Default: redaction ON.
#   --no-appendix       Skip the JSON appendix at end of report.
#   --no-in-vm          Skip the in-VM --self-test-json / --safe-test-json /
#                       freeze-log fetches.
#   --help              Show this usage block and exit.
#
# Safety:
#  - Auto-thaw trap: if the script is interrupted (Ctrl-C, crash) between
#    fsfreeze-freeze and fsfreeze-thaw, an EXIT/INT/TERM handler issues the
#    matching thaw so the VM never lingers frozen waiting on the 10-minute
#    in-agent safety net.
#  - PVE preflights catch concurrent backups (lock=backup), cross-node
#    VMs (qm only talks to local VMs), and missing root privilege before
#    any state-changing operation runs.
#
# Design notes:
#  - Every check that cannot *prove* success reports FAIL, never PASS. A
#    check that passes on data it could not parse manufactures false
#    confidence.
#  - Guest-agent responses are JSON; parsed with Perl JSON::PP (core
#    module on every Proxmox host and stock macOS), never scraped out of
#    formatted CLI text.
#  - The frozen-state behavioural check inspects *response content*, not
#    `qm` exit code (see docs/research/UPSTREAM_NOTES.md Target 4).
#  - 'set -e' is intentionally NOT used: a verification script must run
#    every check, not abort on the first failure.

# --- Argument parsing ------------------------------------------------------
TRANSPORT=""
VMID=""
QGA_SOCKET=""
AGENT_PATH="/usr/local/bin/mac-guest-agent"
LOG_PATH="/var/log/mac-guest-agent.log"
EXEC_TIMEOUT=30
REDACT=1
APPENDIX=1
IN_VM=1

usage() {
    # Print the header comment block (lines 2 through the blank line
    # just before "# --- Argument parsing ---").
    awk '
        NR==1                                    { next }
        /^# --- Argument parsing/                { exit }
        /^#/                                     { sub(/^# ?/, ""); print; next }
        { print }
    ' "$0"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --transport)      TRANSPORT="$2";    shift 2 ;;
        --qga-socket)     QGA_SOCKET="$2";   shift 2 ;;
        --agent-path)     AGENT_PATH="$2";   shift 2 ;;
        --log-path)       LOG_PATH="$2";     shift 2 ;;
        --exec-timeout)   EXEC_TIMEOUT="$2"; shift 2 ;;
        --no-redact)      REDACT=0;          shift ;;
        --no-appendix)    APPENDIX=0;        shift ;;
        --no-in-vm)       IN_VM=0;           shift ;;
        --help|-h)        usage 0 ;;
        --)               shift; break ;;
        -*)               echo "Unknown option: $1" >&2; usage 2 ;;
        *)
            if [ -z "$VMID" ]; then VMID="$1"; else
                echo "Unexpected extra argument: $1" >&2; usage 2
            fi
            shift ;;
    esac
done

if [ -z "$VMID" ] && [ "$TRANSPORT" != "qga-socket" ]; then
    echo "Error: <identifier> is required." >&2
    usage 2
fi

# --- Helpers (transport-agnostic) -----------------------------------------

PASS=0
FAIL=0
FROZE=""                  # set to "1" while the agent is frozen; cleared on thaw
HOST_CHECKS_JSON='[]'     # accumulated structured records for the appendix
SECTION=""                # current section name, attached to each record

# redact: apply PII redaction to a stream when REDACT=1.
#   - IPv4 addresses → <REDACTED-IPV4>
#   - 6-octet MAC addresses → <REDACTED-MAC>
#   - The supplied identifier → <REDACTED-VMID> in known contexts
#     ("VM ID: <id>", "VM <id>", "vmid <id>", "\"vmid\":<id>" in JSON).
#     Bare numeric matches NOT redacted — would eat block counts /
#     command counts / port numbers.
# Implemented in Perl because BSD sed (macOS — the UTM transport host)
# doesn't support \b word boundaries under -E, and we want one redaction
# implementation across Linux/macOS hosts.
redact() {
    if [ "$REDACT" -eq 0 ]; then
        cat
    else
        REDACT_VMID="$VMID" perl -ne '
            s/\b[0-9]{1,3}(?:\.[0-9]{1,3}){3}\b/<REDACTED-IPV4>/g;
            s/\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b/<REDACTED-MAC>/g;
            if (length $ENV{REDACT_VMID}) {
                my $v = quotemeta $ENV{REDACT_VMID};
                s/(VM ID: |VM |vmid )$v\b/$1<REDACTED-VMID>/g;
                s/("vmid"\s*:\s*)"?$v"?/$1"<REDACTED-VMID>"/g;
            }
            print;
        '
    fi
}

emit() { printf '%s\n' "$*" | redact; }

# record <level> <name> [detail] — append a structured record to the
# in-memory HOST_CHECKS_JSON array. Section comes from $SECTION.
record() {
    local level="$1" name="$2" detail="${3:-}"
    HOST_CHECKS_JSON=$(printf '%s' "$HOST_CHECKS_JSON" | REC_SECTION="$SECTION" REC_LEVEL="$level" REC_NAME="$name" REC_DETAIL="$detail" perl -MJSON::PP -e '
        local $/;
        my $arr = decode_json(scalar <STDIN>);
        push @$arr, {
            section => $ENV{REC_SECTION},
            level   => $ENV{REC_LEVEL},
            name    => $ENV{REC_NAME},
            detail  => $ENV{REC_DETAIL},
        };
        print encode_json($arr);
    ' 2>/dev/null || printf '%s' "$HOST_CHECKS_JSON")
}

pass() { emit "  PASS  $1"; record pass "$1" ""; PASS=$((PASS + 1)); }
fail() { emit "  FAIL  $1"; record fail "$1" ""; FAIL=$((FAIL + 1)); }
info() { emit "  INFO  $1"; record info "$1" ""; }

section() {
    SECTION="$1"
    emit ""
    emit "--- $1 ---"
}

# Defined up here (not at the bottom of the script) because the
# VM-not-running early-exit branch calls them — bash binds functions
# when the executor reaches the definition line, not at parse time.
summary() {
    emit ""
    emit "==================================="
    emit "Results: $PASS passed, $FAIL failed"
    if [ "$FAIL" -eq 0 ]; then
        emit "Status: ALL CHECKS PASSED"
    else
        emit "Status: ISSUES FOUND"
    fi
}

emit_appendix() {
    PASS_OUT="$PASS" FAIL_OUT="$FAIL" \
    VMID_OUT="$VMID" AGENT_PATH_OUT="$AGENT_PATH" LOG_PATH_OUT="$LOG_PATH" \
    TRANSPORT_OUT="$TRANSPORT" \
    HOST_CHECKS="$HOST_CHECKS_JSON" \
    SELFTEST="$SELFTEST_JSON" SAFETEST="$SAFETEST_JSON" \
    FREEZE_LOG="$FREEZE_LOG_TAIL" SCRIPT_VERSION="2026-05-23-verify-v1" \
    perl -MJSON::PP -e '
        sub maybe_decode {
            my $s = shift;
            return undef unless defined($s) && length($s);
            my $d = eval { decode_json($s) };
            return $@ ? { _raw_unparseable => $s } : $d;
        }
        my $appendix = {
            schema_version    => "1.0",
            script_version    => $ENV{SCRIPT_VERSION},
            generated_at      => scalar(gmtime) . " UTC",
            transport         => $ENV{TRANSPORT_OUT},
            vmid              => $ENV{VMID_OUT},
            agent_path        => $ENV{AGENT_PATH_OUT},
            log_path          => $ENV{LOG_PATH_OUT},
            counts            => {
                passed => $ENV{PASS_OUT} + 0,
                failed => $ENV{FAIL_OUT} + 0,
            },
            host_checks       => maybe_decode($ENV{HOST_CHECKS}) // [],
            in_vm_selftest    => maybe_decode($ENV{SELFTEST}),
            in_vm_safetest    => maybe_decode($ENV{SAFETEST}),
            freeze_log_tail   => $ENV{FREEZE_LOG} // "",
        };
        print JSON::PP->new->pretty->canonical->encode($appendix);
    ' 2>/dev/null
}

# json_query <json-string> <perl-expression>
# Decodes <json-string> and evaluates <perl-expression> with $d bound to
# the decoded structure; prints the result. Empty output on parse failure.
json_query() {
    printf '%s' "$1" | perl -MJSON::PP -e '
        local $/;
        my $d = eval { decode_json(scalar <STDIN>) };
        exit 2 if $@ || !defined $d;
        my $out = eval $ARGV[0];
        exit 3 if $@;
        print $out if defined $out;
    ' -- "$2" 2>/dev/null
}

# --- Transport: PVE -------------------------------------------------------

pve_describe() {
    printf 'pve (qm — Proxmox VE host-side CLI); VMID=%s' "$VMID"
}

pve_preflight() {
    # 1. qm and perl on PATH.
    for tool in qm perl pvesh; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            fail "preflight — '$tool' not found (need a Proxmox VE host)"
            return 1
        fi
    done

    # 2. Root (qm requires it for most operations).
    if [ "$(id -u)" -ne 0 ]; then
        fail "preflight — must run as root (qm requires root for agent commands)"
        return 1
    fi

    # 3. VMID is numeric.
    if ! [[ "$VMID" =~ ^[0-9]+$ ]]; then
        fail "preflight — VMID '$VMID' is not numeric (PVE VMIDs are integers)"
        return 1
    fi

    # 4. Cluster locality: in a multi-node cluster, qm only talks to VMs on
    # this node. Identify the VM's home node and compare to ours.
    local cluster_json this_node vm_node
    cluster_json=$(pvesh get /cluster/resources --type vm --output-format json 2>/dev/null)
    if [ -n "$cluster_json" ]; then
        this_node=$(hostname -s 2>/dev/null || hostname)
        vm_node=$(printf '%s' "$cluster_json" | VMID_LOOKUP="$VMID" perl -MJSON::PP -e '
            local $/;
            my $arr = decode_json(scalar <STDIN>);
            for my $r (@$arr) {
                if (($r->{vmid} // -1) == $ENV{VMID_LOOKUP}) {
                    print $r->{node} // "";
                    exit 0;
                }
            }
        ' 2>/dev/null)
        if [ -n "$vm_node" ] && [ "$vm_node" != "$this_node" ]; then
            fail "preflight — VM $VMID lives on node '$vm_node' (this is '$this_node'); rerun on '$vm_node'"
            return 1
        fi
    fi

    # 5. Backup lock: if vzdump is in progress, abort cleanly.
    local node_for_status status_json lock_state
    node_for_status="${vm_node:-$(hostname -s 2>/dev/null || hostname)}"
    status_json=$(pvesh get "/nodes/$node_for_status/qemu/$VMID/status/current" --output-format json 2>/dev/null)
    if [ -n "$status_json" ]; then
        lock_state=$(json_query "$status_json" '$d->{lock} // ""')
        if [ "$lock_state" = "backup" ]; then
            fail "preflight — VM $VMID is currently being backed up (lock=backup); rerun after vzdump completes"
            return 1
        elif [ -n "$lock_state" ]; then
            info "preflight — VM $VMID has lock=$lock_state (proceeding cautiously)"
        fi
    fi

    return 0
}

pve_vm_state() {
    local state
    state=$(qm status "$VMID" 2>/dev/null | awk '{print $2}')
    printf '%s' "${state:-unknown}"
}

pve_config_summary() {
    local conf="/etc/pve/qemu-server/$VMID.conf"
    if [ ! -f "$conf" ]; then
        fail "VM config not found: $conf"
        return
    fi
    local agent_line
    agent_line=$(grep -E '^agent:' "$conf")
    if echo "$agent_line" | grep -qE '(^|[ ,])enabled=1([ ,]|$)'; then
        if echo "$agent_line" | grep -qE '(^|[ ,])type=isa([ ,]|$)'; then
            pass "agent: enabled=1, type=isa"
        else
            fail "agent enabled but type=isa not set — Apple's VirtIO agent may answer instead"
        fi
    else
        fail "guest agent not enabled in VM config"
    fi
    if grep -qE 'discard=on' "$conf"; then
        pass "discard=on (TRIM enabled)"
    else
        info "discard not enabled (optional — needed for guest-fstrim)"
    fi
    if grep -qE 'ssd=1' "$conf"; then
        pass "ssd=1 (SSD emulation)"
    else
        info "ssd=1 not set (optional — needed for guest-fstrim)"
    fi
}

# pve_qga_cmd <command> [arg ...]
# Runs `qm agent <vmid> <command> [args]` and prints the JSON response.
# Empty output on failure.
pve_qga_cmd() {
    qm agent "$VMID" "$@" 2>/dev/null
}

# pve_guest_exec_json <path> [args...]
# Runs `qm guest exec --output-format json --timeout N <vmid> -- <path> [args]`
# and prints the envelope JSON ({exited, exitcode, out-data, err-data}).
# Empty output on parse failure.
pve_guest_exec_json() {
    local raw
    raw=$(qm guest exec --timeout "$EXEC_TIMEOUT" --output-format json "$VMID" -- "$@" 2>/dev/null)
    if printf '%s' "$raw" | perl -MJSON::PP -e 'local $/; eval { decode_json(scalar <STDIN>) }; exit($@ ? 1 : 0)' 2>/dev/null; then
        printf '%s' "$raw"
    fi
}

# --- Transport dispatch ---------------------------------------------------

# Detect the transport from environment when --transport is omitted.
auto_detect_transport() {
    if [ -n "$TRANSPORT" ]; then
        return 0
    fi
    if [[ "$VMID" =~ ^[0-9]+$ ]] && command -v qm >/dev/null 2>&1 \
        && [ -f "/etc/pve/qemu-server/$VMID.conf" ]; then
        TRANSPORT="pve"
        return 0
    fi
    # libvirt and utm auto-detect arrive in commits 2 and 3.
    echo "Error: could not auto-detect transport. Pass --transport pve|libvirt|utm|qga-socket explicitly." >&2
    exit 2
}

# Bind the four function pointers to the chosen transport's implementations.
bind_transport() {
    case "$TRANSPORT" in
        pve)
            transport_describe=pve_describe
            transport_preflight=pve_preflight
            transport_vm_state=pve_vm_state
            transport_config_summary=pve_config_summary
            transport_qga_cmd=pve_qga_cmd
            transport_guest_exec_json=pve_guest_exec_json
            ;;
        libvirt|utm|qga-socket)
            echo "Error: transport '$TRANSPORT' not yet implemented (lands in a subsequent commit)." >&2
            exit 2
            ;;
        *)
            echo "Error: unknown transport '$TRANSPORT' (expected: pve|libvirt|utm|qga-socket)." >&2
            exit 2
            ;;
    esac
}

# --- Auto-thaw safety trap ------------------------------------------------
# If anything between fsfreeze-freeze and fsfreeze-thaw kills the script,
# this trap fires and issues the thaw before exit. The agent has its own
# 10-minute auto-thaw safety net; this just makes recovery immediate.
emergency_thaw() {
    local exit_code=$?
    if [ -n "$FROZE" ] && [ -n "$transport_qga_cmd" ]; then
        emit ""
        emit "*** Aborting while VM is frozen — issuing emergency fsfreeze-thaw ***"
        "$transport_qga_cmd" fsfreeze-thaw >/dev/null 2>&1
    fi
    exit "$exit_code"
}
trap emergency_thaw EXIT INT TERM

# --- Main flow ------------------------------------------------------------

auto_detect_transport
bind_transport

emit "mac-guest-agent — Host-side Verification"
emit "=========================================="
emit "Transport: $($transport_describe)"
emit "VM ID: $VMID"
emit "Started: $(date -Iseconds 2>/dev/null || date)"
emit ""

# --- Preflight ------------------------------------------------------------
section "Preflight"
if ! "$transport_preflight"; then
    # Render summary + appendix even on preflight failure so the contributor
    # has something concrete to paste back.
    :
fi

# --- Configuration --------------------------------------------------------
section "Configuration"
"$transport_config_summary"

# --- VM state -------------------------------------------------------------
section "VM State"
VM_STATE=$("$transport_vm_state")
if [ "$VM_STATE" = "running" ]; then
    pass "VM $VMID is running"
else
    fail "VM $VMID is not running (state: ${VM_STATE:-unknown}) — cannot verify the guest agent"
    summary
    if [ "$APPENDIX" -eq 1 ]; then
        emit ""
        emit "==================================="
        emit "JSON Appendix (paste into docs/evidence/<version>/verify.json)"
        emit "==================================="
        emit_appendix | redact
    fi
    trap - EXIT INT TERM
    exit 1
fi

# --- Agent communication --------------------------------------------------
section "Agent Communication"

if "$transport_qga_cmd" ping >/dev/null 2>&1; then
    pass "ping"
else
    fail "ping — agent not responding"
fi

OSINFO=$("$transport_qga_cmd" get-osinfo 2>/dev/null)
PRETTY=$(json_query "$OSINFO" '$d->{"pretty-name"} // $d->{name}')
if [ -n "$PRETTY" ]; then
    pass "get-osinfo — $PRETTY"
else
    fail "get-osinfo — no valid response"
fi

NETINFO=$("$transport_qga_cmd" network-get-interfaces 2>/dev/null)
IFCOUNT=$(json_query "$NETINFO" 'ref $d eq "ARRAY" ? scalar @$d : ""')
if [ -n "$IFCOUNT" ] && [ "$IFCOUNT" -gt 0 ] 2>/dev/null; then
    IP=$(json_query "$NETINFO" \
        'my $ip; for my $i (@$d) { for my $a (@{$i->{"ip-addresses"} || []}) { $ip ||= $a->{"ip-address"} } } $ip // "no IP"')
    pass "network-get-interfaces — $IFCOUNT interface(s), IP $IP"
else
    fail "network-get-interfaces — no valid response"
fi

AGENTINFO=$("$transport_qga_cmd" info 2>/dev/null)
CMDCOUNT=$(json_query "$AGENTINFO" 'scalar @{$d->{supported_commands} || []}')
AGENTVER=$(json_query "$AGENTINFO" '$d->{version} // ""')
if [ -n "$CMDCOUNT" ] && [ "$CMDCOUNT" -ge 40 ] 2>/dev/null; then
    pass "info — $CMDCOUNT commands registered${AGENTVER:+, agent v$AGENTVER}"
elif [ -n "$CMDCOUNT" ] && [ "$CMDCOUNT" -gt 0 ] 2>/dev/null; then
    fail "info — only $CMDCOUNT commands (expected ~45; a different agent may be answering)"
else
    fail "info — no valid response from agent"
fi

# --- Memory (from the guest agent) ----------------------------------------
section "Memory (guest agent)"
BLKINFO=$("$transport_qga_cmd" get-memory-block-info 2>/dev/null)
BLKSIZE=$(json_query "$BLKINFO" '$d->{size}')
BLOCKS=$("$transport_qga_cmd" get-memory-blocks 2>/dev/null)
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

# --- Freeze / thaw --------------------------------------------------------
section "Freeze / Thaw"
# macOS has no FIFREEZE; freeze is per-FS dispatch (see docs/design/FREEZE_SEMANTICS.md).
# Beyond the command path this verifies the frozen STATE behaviourally:
# while frozen, a non-freeze command must be rejected BY CONTENT (PVE's
# register_command dispatcher exits 0 on QGA errors — see Target 4 in
# docs/research/UPSTREAM_NOTES.md — so we cannot use exit code).
FREEZE_LOG_TAIL=""

FREEZE=$("$transport_qga_cmd" fsfreeze-freeze 2>/dev/null)
FROZEN_N=$(echo "$FREEZE" | grep -oE '[0-9]+' | head -1)
if [ -n "$FROZEN_N" ] && [ "$FROZEN_N" -ge 1 ]; then
    FROZE=1                                        # arm the auto-thaw trap
    pass "fsfreeze-freeze — $FROZEN_N filesystem(s) frozen"

    STATUS=$("$transport_qga_cmd" fsfreeze-status 2>/dev/null)
    if echo "$STATUS" | grep -qw "frozen"; then
        pass "fsfreeze-status — reports frozen"
    else
        fail "fsfreeze-status — not reported frozen after freeze"
    fi

    FROZEN_RESP=$("$transport_qga_cmd" get-osinfo 2>&1)
    if echo "$FROZEN_RESP" | grep -qE '"pretty-name"|pretty-name:'; then
        fail "frozen state — agent served get-osinfo while frozen (NOT genuinely gated)"
    elif echo "$FROZEN_RESP" | grep -qiE 'Command not allowed while filesystem is frozen|"error"'; then
        pass "frozen state — non-freeze command rejected by content"
    else
        info "frozen state — ambiguous response: $(printf '%s' "$FROZEN_RESP" | head -c 200)"
    fi

    THAW=$("$transport_qga_cmd" fsfreeze-thaw 2>/dev/null)
    THAWED_N=$(echo "$THAW" | grep -oE '[0-9]+' | head -1)
    if [ -n "$THAWED_N" ] && [ "$THAWED_N" -ge 1 ]; then
        FROZE=""                                    # disarm the trap
        pass "fsfreeze-thaw — $THAWED_N filesystem(s) thawed"
    else
        fail "fsfreeze-thaw — thaw not confirmed; the VM filesystem may still be frozen"
    fi

    POST_RESP=$("$transport_qga_cmd" get-osinfo 2>&1)
    if echo "$POST_RESP" | grep -qE '"pretty-name"|pretty-name:'; then
        pass "post-thaw — agent answers get-osinfo normally again"
    else
        fail "post-thaw — get-osinfo response did not carry pretty-name after thaw"
    fi

    # Per-event freeze INFO line from the agent log (Phase 2 Q3 surface).
    if [ "$IN_VM" -eq 1 ]; then
        LOG_RAW=$("$transport_guest_exec_json" /usr/bin/tail -n 200 "$LOG_PATH")
        if [ -n "$LOG_RAW" ]; then
            LOG_TEXT=$(json_query "$LOG_RAW" '$d->{"out-data"} // ""')
            FREEZE_LOG_TAIL=$(printf '%s' "$LOG_TEXT" | grep -E 'Filesystem frozen:' | tail -n 1)
            if [ -n "$FREEZE_LOG_TAIL" ]; then
                pass "freeze log — $FREEZE_LOG_TAIL"
            else
                info "freeze log — '$LOG_PATH' tail had no 'Filesystem frozen:' INFO line in the last 200 lines"
            fi
        else
            info "freeze log — guest-exec tail $LOG_PATH returned no JSON (binary or log missing?)"
        fi
    fi
elif [ -n "$FROZEN_N" ]; then
    fail "fsfreeze-freeze — froze 0 filesystems (freeze had no effect)"
else
    fail "fsfreeze-freeze — no response"
fi

# --- In-VM diagnostics (--self-test-json + --safe-test-json) --------------
SELFTEST_JSON=""
SAFETEST_JSON=""
if [ "$IN_VM" -eq 1 ]; then
    section "In-VM Diagnostics"

    SELFTEST_RAW=$("$transport_guest_exec_json" "$AGENT_PATH" --self-test-json)
    if [ -n "$SELFTEST_RAW" ]; then
        SELFTEST_JSON=$(json_query "$SELFTEST_RAW" '$d->{"out-data"} // ""')
        if [ -n "$SELFTEST_JSON" ]; then
            ST_STATUS=$(json_query "$SELFTEST_JSON" '$d->{status} // ""')
            ST_ERR=$(json_query   "$SELFTEST_JSON" '$d->{errors}   // ""')
            ST_WARN=$(json_query  "$SELFTEST_JSON" '$d->{warnings} // ""')
            ST_PASS=$(json_query  "$SELFTEST_JSON" '$d->{passes}   // ""')
            ST_VER=$(json_query   "$SELFTEST_JSON" '$d->{agent_version} // ""')
            if [ "$ST_STATUS" = "pass" ]; then
                pass "--self-test-json — agent v${ST_VER}: $ST_PASS pass / $ST_WARN warn / $ST_ERR err"
            elif [ -n "$ST_STATUS" ]; then
                fail "--self-test-json — status=$ST_STATUS (errors=$ST_ERR, warnings=$ST_WARN)"
            else
                fail "--self-test-json — could not parse status field"
            fi

            FD_DISCRIM=$(json_query "$SELFTEST_JSON" '$d->{freeze_dispatch}->{cpustats_discriminator} // ""')
            FD_APFS=$(json_query    "$SELFTEST_JSON" '$d->{freeze_dispatch}->{per_fstypename}->{apfs} // ""')
            FD_ZFS_CLI=$(json_query "$SELFTEST_JSON" '$d->{freeze_dispatch}->{zfs_cli_available} // ""')
            if [ "$FD_APFS" = "tmutil_snapshot+f_fullfsync" ] && [ "$FD_DISCRIM" = "linux" ]; then
                pass "freeze_dispatch contract — apfs=$FD_APFS, cpustats=$FD_DISCRIM, zfs_cli=$FD_ZFS_CLI"
            else
                fail "freeze_dispatch contract — apfs='$FD_APFS' cpustats='$FD_DISCRIM' (expected 'tmutil_snapshot+f_fullfsync' / 'linux')"
            fi
        else
            fail "--self-test-json — guest-exec returned no out-data"
        fi
    else
        info "--self-test-json — guest-exec $AGENT_PATH failed (binary missing or guest-exec disabled?)"
    fi

    SAFETEST_RAW=$("$transport_guest_exec_json" "$AGENT_PATH" --safe-test-json)
    if [ -n "$SAFETEST_RAW" ]; then
        SAFETEST_JSON=$(json_query "$SAFETEST_RAW" '$d->{"out-data"} // ""')
        if [ -n "$SAFETEST_JSON" ]; then
            SA_PASS=$(json_query "$SAFETEST_JSON" '$d->{summary}->{passed} // $d->{passed} // ""')
            SA_FAIL=$(json_query "$SAFETEST_JSON" '$d->{summary}->{failed} // $d->{failed} // ""')
            SA_TOTAL=$(json_query "$SAFETEST_JSON" '$d->{summary}->{total}  // $d->{total}  // ""')
            if [ -n "$SA_PASS" ] && [ -n "$SA_FAIL" ]; then
                if [ "$SA_FAIL" -eq 0 ] 2>/dev/null; then
                    pass "--safe-test-json — $SA_PASS/${SA_TOTAL:-?} read-only commands passed"
                else
                    fail "--safe-test-json — $SA_FAIL/${SA_TOTAL:-?} read-only commands failed"
                fi
            else
                fail "--safe-test-json — could not parse pass/fail counts"
            fi
        else
            fail "--safe-test-json — guest-exec returned no out-data"
        fi
    else
        info "--safe-test-json — guest-exec $AGENT_PATH failed (binary missing or guest-exec disabled?)"
    fi
fi

# --- Summary + JSON appendix ----------------------------------------------
# (summary() and emit_appendix() are defined above the main flow so the
#  early-exit branches can call them too.)

summary

if [ "$APPENDIX" -eq 1 ]; then
    emit ""
    emit "==================================="
    emit "JSON Appendix (paste into docs/evidence/<version>/verify.json)"
    emit "==================================="
    emit_appendix | redact
fi

# Clear the trap before normal exit so it doesn't fire spuriously.
trap - EXIT INT TERM
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
