#!/bin/bash
# PVE Host-Side Verification Script
#
# Run from the Proxmox VE host to verify a macOS VM's guest agent end-to-end:
#   ./pve-verify.sh <vmid> [options]
#
# Options:
#   --no-redact         Disable PII redaction (IPs, MAC addresses, VM ID).
#                       Default: redaction ON.
#   --no-appendix       Skip the structured JSON appendix at end of report.
#                       Default: appendix emitted.
#   --no-in-vm          Skip the in-VM --self-test-json / --safe-test-json /
#                       freeze-log fetches. Useful when the guest binary is
#                       missing or guest-exec is disabled. Default: in-VM
#                       diagnostics ARE run.
#   --agent-path PATH   Guest path to the mac-guest-agent binary used for
#                       in-VM diagnostics. Default: /usr/local/bin/mac-guest-agent
#   --log-path PATH     Guest path to the agent log used for the freeze-event
#                       INFO line fetch. Default: /var/log/mac-guest-agent.log
#   --exec-timeout SEC  qm guest exec timeout for in-VM commands.
#                       Default: 30
#   --help              Show usage and exit.
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
#  - The frozen-state behavioural check inspects *response content*, not
#    `qm` exit code. Per docs/research/UPSTREAM_NOTES.md Target 4, PVE's
#    `register_command` dispatcher used by `qm agent <cmd>` wraps QGA
#    errors as `{result:{error:{...}}}` and the CLI exits 0 regardless,
#    so an exit-code check cannot distinguish an honest rejection from a
#    silently-served response.
#  - The in-VM diagnostics (--self-test-json, --safe-test-json, freeze-log
#    tail) are run via `qm guest exec`. This is the Phase 3 "one-shot"
#    flow per docs/PLAN.md: a single host-side invocation produces both
#    the host-side checks and the in-VM diagnostics, and the structured
#    appendix is the artefact that gets pasted into docs/evidence/<ver>/.
#
# 'set -e' is intentionally NOT used: a verification script must run every
# check, not abort on the first failure.

# --- Argument parsing ------------------------------------------------------
VMID=""
REDACT=1
APPENDIX=1
IN_VM=1
AGENT_PATH="/usr/local/bin/mac-guest-agent"
LOG_PATH="/var/log/mac-guest-agent.log"
EXEC_TIMEOUT=30

usage() {
    sed -n '2,47p' "$0"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-redact)      REDACT=0;        shift ;;
        --no-appendix)    APPENDIX=0;      shift ;;
        --no-in-vm)       IN_VM=0;         shift ;;
        --agent-path)     AGENT_PATH="$2"; shift 2 ;;
        --log-path)       LOG_PATH="$2";   shift 2 ;;
        --exec-timeout)   EXEC_TIMEOUT="$2"; shift 2 ;;
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

if [ -z "$VMID" ]; then
    echo "Error: <vmid> is required." >&2
    usage 2
fi

# --- Helpers ---------------------------------------------------------------

PASS=0
FAIL=0
HOST_CHECKS_JSON='[]'      # accumulated structured checks for the appendix

# redact: apply PII redaction to a stream when REDACT=1.
#  - IPv4 addresses become <REDACTED-IPV4>.
#  - 6-octet MAC addresses become <REDACTED-MAC>.
#  - The VMID (used as a word) becomes <REDACTED-VMID> when wrapped in
#    obvious contexts ("VM ID: <vmid>", "VM <vmid>", "vmid <vmid>",
#    or as a "vmid":<n> JSON value). Bare numeric matches are not
#    redacted to avoid eating memory block counts, command counts, etc.
redact() {
    if [ "$REDACT" -eq 0 ]; then
        cat
    else
        sed -E \
            -e 's/\b[0-9]{1,3}(\.[0-9]{1,3}){3}\b/<REDACTED-IPV4>/g' \
            -e 's/\b[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}\b/<REDACTED-MAC>/g' \
            -e "s/(VM ID: |VM |vmid )$VMID\\b/\\1<REDACTED-VMID>/g" \
            -e "s/(\"vmid\"[[:space:]]*:[[:space:]]*)\"?$VMID\"?/\\1\"<REDACTED-VMID>\"/g"
    fi
}

emit() { printf '%s\n' "$*" | redact; }

# pass/fail/info: human-line emission + structured record for the appendix.
# Args: $1 = section, $2 = name, $3 = detail (optional).
pass_in_section=""
record() {
    # record <level> <name> <detail>
    local level="$1" name="$2" detail="${3:-}"
    HOST_CHECKS_JSON=$(printf '%s' "$HOST_CHECKS_JSON" | perl -MJSON::PP -e '
        local $/;
        my $arr = decode_json(scalar <STDIN>);
        push @$arr, {
            section => $ENV{REC_SECTION},
            level   => $ENV{REC_LEVEL},
            name    => $ENV{REC_NAME},
            detail  => $ENV{REC_DETAIL},
        };
        print encode_json($arr);
    ' REC_SECTION="$pass_in_section" REC_LEVEL="$level" REC_NAME="$name" REC_DETAIL="$detail" 2>/dev/null || printf '%s' "$HOST_CHECKS_JSON")
}

pass() { emit "  PASS  $1"; record pass "$1" ""; PASS=$((PASS + 1)); }
fail() { emit "  FAIL  $1"; record fail "$1" ""; FAIL=$((FAIL + 1)); }
info() { emit "  INFO  $1"; record info "$1" ""; }

section() {
    pass_in_section="$1"
    emit ""
    emit "--- $1 ---"
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

# guest_exec_json <vmid> <path> [args...]
#
# Runs a guest binary via `qm guest exec --output-format json`, prints the
# raw JSON envelope on stdout. Prints nothing on failure. The envelope from
# PVE is `{exited, exitcode, out-data, err-data, signal?}` with out-data
# and err-data already decoded by `qm` from the QGA base64 payload.
guest_exec_json() {
    local vmid="$1"; shift
    local raw
    raw=$(qm guest exec --timeout "$EXEC_TIMEOUT" --output-format json "$vmid" -- "$@" 2>/dev/null)
    if printf '%s' "$raw" | perl -MJSON::PP -e 'local $/; eval { decode_json(scalar <STDIN>) }; exit($@ ? 1 : 0)' 2>/dev/null; then
        printf '%s' "$raw"
    fi
}

# --- Preflight -------------------------------------------------------------
for tool in qm perl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: '$tool' not found — run this script on the Proxmox VE host." >&2
        exit 2
    fi
done

emit "PVE macOS Guest Agent Verification"
emit "==================================="
emit "VM ID: $VMID"
emit "Started: $(date -Iseconds 2>/dev/null || date)"
emit ""

# --- Configuration ---------------------------------------------------------
section "Configuration"
CONF="/etc/pve/qemu-server/$VMID.conf"
if [ -f "$CONF" ]; then
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
section "VM State"
VM_STATE=$(qm status "$VMID" 2>/dev/null | awk '{print $2}')
if [ "$VM_STATE" = "running" ]; then
    pass "VM $VMID is running"
else
    fail "VM $VMID is not running (state: ${VM_STATE:-unknown}) — cannot verify the guest agent"
    [ "$APPENDIX" -eq 1 ] && emit_appendix
    summary
    exit 1
fi

# --- Agent communication ---------------------------------------------------
section "Agent Communication"

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
section "Memory (guest agent)"
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
section "Freeze / Thaw"
# macOS has no FIFREEZE ioctl; the agent's freeze is a per-FS dispatch:
# tmutil snapshot (APFS), F_FULLFSYNC (HFS+/FAT/exFAT/UDF/NTFS),
# zfs snapshot (OpenZFS when CLI present), and categorical skips for
# network and special filesystems. See docs/design/FREEZE_SEMANTICS.md.
# Beyond the command path this verifies the frozen STATE behaviourally:
# while frozen, the agent must REJECT non-freeze commands by content
# (NOT by exit code — see header note re Target 4), and must resume
# normal operation after thaw.

FREEZE_LOG_TAIL=""

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

    # Behavioural proof — inspect the *response content*, not the exit
    # code. PVE's `register_command` dispatcher wraps QGA errors as
    # `{result:{error:{...}}}` and `qm agent` exits 0 either way
    # (docs/research/UPSTREAM_NOTES.md Target 4), so an exit-code check
    # cannot distinguish honest rejection from a silently-served reply.
    FROZEN_RESP=$(qm agent "$VMID" get-osinfo 2>&1)
    if echo "$FROZEN_RESP" | grep -qE '"pretty-name"|pretty-name:'; then
        fail "frozen state — agent served get-osinfo while frozen (NOT genuinely gated)"
    elif echo "$FROZEN_RESP" | grep -qiE 'Command not allowed while filesystem is frozen|"error"'; then
        pass "frozen state — non-freeze command rejected by content (response carried error)"
    else
        info "frozen state — ambiguous response: $(printf '%s' "$FROZEN_RESP" | head -c 200)"
    fi

    THAW=$(qm guest cmd "$VMID" fsfreeze-thaw 2>/dev/null)
    THAWED_N=$(echo "$THAW" | grep -oE '[0-9]+' | head -1)
    if [ -n "$THAWED_N" ] && [ "$THAWED_N" -ge 1 ]; then
        pass "fsfreeze-thaw — $THAWED_N filesystem(s) thawed"
    else
        fail "fsfreeze-thaw — thaw not confirmed; the VM filesystem may still be frozen"
    fi

    # Post-thaw behavioural proof — same content-inspection rule. A
    # successful get-osinfo MUST contain "pretty-name".
    POST_RESP=$(qm agent "$VMID" get-osinfo 2>&1)
    if echo "$POST_RESP" | grep -qE '"pretty-name"|pretty-name:'; then
        pass "post-thaw — agent answers get-osinfo normally again"
    else
        fail "post-thaw — get-osinfo response did not carry pretty-name after thaw"
    fi

    # Fetch the per-event "Filesystem frozen:" INFO line from the agent
    # log so the operator sees the per-treatment breakdown (Phase 2 Q3).
    # Uses `qm guest exec` for tail + grep; degrades to INFO on failure
    # (the log may not exist if the agent runs in a non-daemon mode, or
    # the contributor may have a custom --logfile path).
    if [ "$IN_VM" -eq 1 ]; then
        LOG_RAW=$(guest_exec_json "$VMID" /usr/bin/tail -n 200 "$LOG_PATH")
        if [ -n "$LOG_RAW" ]; then
            LOG_TEXT=$(json_query "$LOG_RAW" '$d->{"out-data"} // ""')
            FREEZE_LOG_TAIL=$(printf '%s' "$LOG_TEXT" | grep -E 'Filesystem frozen:' | tail -n 1)
            if [ -n "$FREEZE_LOG_TAIL" ]; then
                pass "freeze log — $FREEZE_LOG_TAIL"
            else
                info "freeze log — '$LOG_PATH' tail had no 'Filesystem frozen:' INFO line in the last 200 lines"
            fi
        else
            info "freeze log — qm guest exec tail $LOG_PATH returned no JSON (binary or log missing?)"
        fi
    fi
elif [ -n "$FROZEN_N" ]; then
    fail "fsfreeze-freeze — froze 0 filesystems (freeze had no effect)"
else
    fail "fsfreeze-freeze — no response"
fi

# --- In-VM diagnostics (--self-test-json + --safe-test-json) ---------------
SELFTEST_JSON=""
SAFETEST_JSON=""
if [ "$IN_VM" -eq 1 ]; then
    section "In-VM Diagnostics"

    SELFTEST_RAW=$(guest_exec_json "$VMID" "$AGENT_PATH" --self-test-json)
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

            # Static dispatch contract — make sure the binary advertises
            # the per-FS dispatch policy and the cpustats discriminator
            # that Phase 2 Q3/Q4 designed. Surfacing this here makes the
            # contract drift visible to a contributor running pve-verify
            # against any release.
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
        info "--self-test-json — qm guest exec $AGENT_PATH failed (binary missing or guest-exec disabled?)"
    fi

    SAFETEST_RAW=$(guest_exec_json "$VMID" "$AGENT_PATH" --safe-test-json)
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
        info "--safe-test-json — qm guest exec $AGENT_PATH failed (binary missing or guest-exec disabled?)"
    fi
fi

# --- Summary ---------------------------------------------------------------
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

# --- JSON appendix ---------------------------------------------------------
# Builds and emits a single structured JSON object that the contributor
# can paste straight into docs/evidence/<version>/pve-verify.json. The
# in-VM --self-test-json and --safe-test-json outputs are embedded as
# parsed objects (not strings) so downstream consumers can read them
# without a second decode pass.
emit_appendix() {
    PASS_OUT="$PASS" FAIL_OUT="$FAIL" \
    VMID_OUT="$VMID" AGENT_PATH_OUT="$AGENT_PATH" LOG_PATH_OUT="$LOG_PATH" \
    HOST_CHECKS="$HOST_CHECKS_JSON" \
    SELFTEST="$SELFTEST_JSON" SAFETEST="$SAFETEST_JSON" \
    FREEZE_LOG="$FREEZE_LOG_TAIL" SCRIPT_VERSION="2026-05-23-phase3" \
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

summary

if [ "$APPENDIX" -eq 1 ]; then
    emit ""
    emit "==================================="
    emit "JSON Appendix (paste into docs/evidence/<version>/pve-verify.json)"
    emit "==================================="
    emit_appendix | redact
fi

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
