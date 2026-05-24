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
#   --no-env-capture    Skip the Host Environment capture section
#                       (sw_vers / sysctl / kextstat / ioreg / mount /
#                       launchctl / log-file stat). Use when guest-exec
#                       is slow or you only want host-driven checks.
#   --no-freeze         Skip the Freeze/Thaw section entirely. Useful
#                       for cautious contributors who don't want to
#                       freeze a production-ish VM; everything else
#                       still runs (~80% of the evidence).
#   --freeze-cycles N   Number of freeze/thaw cycles to run (default 3).
#                       Multiple cycles catch state leakage between
#                       cycles — a real bug class the prior single-cycle
#                       check missed. Set to 1 for old behaviour.
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
ENV_CAPTURE=1
RUN_FREEZE=1
FREEZE_CYCLES=3

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
        --no-env-capture) ENV_CAPTURE=0;     shift ;;
        --no-freeze)      RUN_FREEZE=0;      shift ;;
        --freeze-cycles)  FREEZE_CYCLES="$2"; shift 2 ;;
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

if ! [[ "$FREEZE_CYCLES" =~ ^[0-9]+$ ]] || [ "$FREEZE_CYCLES" -lt 1 ]; then
    echo "Error: --freeze-cycles must be a positive integer (got: '$FREEZE_CYCLES')." >&2
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
    HOST_ENVIRONMENT="$HOST_ENVIRONMENT_JSON" \
    SELFTEST="$SELFTEST_JSON" SAFETEST="$SAFETEST_JSON" \
    FREEZE_LOG="$FREEZE_LOG_TAIL" \
    FREEZE_CYCLES_LOG="$FREEZE_CYCLES_JSON" \
    FREEZE_CYCLES_COUNT="$FREEZE_CYCLES" \
    MOUNT_XCHECK="$MOUNT_DISPATCH_CROSSCHECK_JSON" \
    SCRIPT_VERSION="2026-05-23-verify-v2" \
    perl -MJSON::PP -e '
        sub maybe_decode {
            my $s = shift;
            return undef unless defined($s) && length($s);
            my $d = eval { decode_json($s) };
            return $@ ? { _raw_unparseable => $s } : $d;
        }
        my $appendix = {
            schema_version             => "2.0",
            script_version             => $ENV{SCRIPT_VERSION},
            generated_at               => scalar(gmtime) . " UTC",
            transport                  => $ENV{TRANSPORT_OUT},
            vmid                       => $ENV{VMID_OUT},
            agent_path                 => $ENV{AGENT_PATH_OUT},
            log_path                   => $ENV{LOG_PATH_OUT},
            freeze_cycles              => $ENV{FREEZE_CYCLES_COUNT} + 0,
            counts                     => {
                passed => $ENV{PASS_OUT} + 0,
                failed => $ENV{FAIL_OUT} + 0,
            },
            host_checks                => maybe_decode($ENV{HOST_CHECKS}) // [],
            host_environment           => maybe_decode($ENV{HOST_ENVIRONMENT}),
            in_vm_selftest             => maybe_decode($ENV{SELFTEST}),
            in_vm_safetest             => maybe_decode($ENV{SAFETEST}),
            freeze_log_tail            => $ENV{FREEZE_LOG} // "",
            freeze_cycles_log          => maybe_decode($ENV{FREEZE_CYCLES_LOG}) // [],
            mount_dispatch_crosscheck  => maybe_decode($ENV{MOUNT_XCHECK}),
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

# --- guest-exec capture helper --------------------------------------------
# gx_capture <path> [args...] — runs a guest binary via the bound
# transport_guest_exec_json primitive, prints the binary's stdout
# (`out-data` field), empty on failure. Used by capture_host_environment
# below for each individual probe.
gx_capture() {
    local raw
    raw=$("$transport_guest_exec_json" "$@")
    [ -z "$raw" ] && return 1
    json_query "$raw" '$d->{"out-data"} // ""'
}

# --- Host environment capture --------------------------------------------
# Captures sw_vers + sysctl + kextstat (filtered) + ioreg (serial nodes
# only) + mount (parsed) + launchctl + log-file stat, all via guest-exec
# on the bound transport. Stores the assembled JSON in
# HOST_ENVIRONMENT_JSON (consumed by emit_appendix at the bottom of the
# script). Each individual probe surfaces as a PASS/INFO line in the
# human-readable section for visibility.
#
# Runs BEFORE the freeze section so the captured mount table reflects
# pre-freeze state and isn't blocked by the freeze command allowlist
# (sw_vers etc. aren't allowlisted, so they'd error during freeze).
HOST_ENVIRONMENT_JSON=""

capture_host_environment() {
    section "Host Environment"

    local sw_vers_raw sysctl_raw kextstat_raw ioreg_raw mount_raw launchd_raw log_stat_raw

    sw_vers_raw=$(gx_capture /usr/bin/sw_vers)
    if [ -n "$sw_vers_raw" ]; then
        pass "sw_vers captured ($(printf '%s' "$sw_vers_raw" | tr '\n' ' ' | head -c 80)...)"
    else
        info "sw_vers — guest-exec failed (binary missing or guest-exec disabled?)"
    fi

    sysctl_raw=$(gx_capture /usr/sbin/sysctl -n hw.model hw.ncpu hw.memsize machdep.cpu.brand_string)
    if [ -n "$sysctl_raw" ]; then
        pass "sysctl captured ($(printf '%s' "$sysctl_raw" | tr '\n' '|' | head -c 80))"
    else
        info "sysctl — guest-exec failed"
    fi

    # kextstat is large; filter inside the guest to the families we care
    # about. /bin/sh -c is reliable on every macOS; awk's piping the
    # output back would also work but adds a shim.
    kextstat_raw=$(gx_capture /bin/sh -c "/usr/sbin/kextstat 2>/dev/null | /usr/bin/grep -iE 'Apple16X50|AppleVirtIO|IOSerialFamily' || true")
    if [ -n "$kextstat_raw" ]; then
        pass "kextstat captured ($(printf '%s' "$kextstat_raw" | wc -l | tr -d ' ') matching kext line(s))"
    else
        info "kextstat — no matching kexts found or guest-exec failed"
    fi

    # ioreg is very large; filter to lines mentioning serial / virtio
    # IOClass matches plus the 5-line context after each match.
    ioreg_raw=$(gx_capture /bin/sh -c "/usr/sbin/ioreg -l -w 0 2>/dev/null | /usr/bin/grep -i -A 5 -E 'IOClass.*Serial|IOClass.*VirtIO' | /usr/bin/head -c 8192 || true")
    if [ -n "$ioreg_raw" ]; then
        pass "ioreg serial nodes captured ($(printf '%s' "$ioreg_raw" | wc -c | tr -d ' ') bytes)"
    else
        info "ioreg — no matching nodes found or guest-exec failed"
    fi

    mount_raw=$(gx_capture /sbin/mount)
    if [ -n "$mount_raw" ]; then
        local mount_count
        mount_count=$(printf '%s' "$mount_raw" | grep -c '^/dev\|^[[:graph:]]\+ on ' || true)
        pass "mount table captured ($(printf '%s' "$mount_raw" | wc -l | tr -d ' ') mount lines)"
    else
        info "mount — guest-exec failed"
    fi

    launchd_raw=$(gx_capture /bin/launchctl list com.macos.guest-agent)
    if [ -n "$launchd_raw" ]; then
        pass "launchd job state captured"
    else
        info "launchctl — com.macos.guest-agent not loaded, or guest-exec failed"
    fi

    log_stat_raw=$(gx_capture /usr/bin/stat -f "size=%z mtime=%Sm name=%N" -t "%Y-%m-%dT%H:%M:%S" "$LOG_PATH")
    if [ -n "$log_stat_raw" ]; then
        pass "log-file stat: $(printf '%s' "$log_stat_raw" | tr -d '\n')"
    else
        info "log-file stat — '$LOG_PATH' not present, or guest-exec failed"
    fi

    # Assemble the captured pieces into a single JSON object. Parsing
    # happens in Perl rather than bash because mount-line / sysctl-line
    # parsing is fiddly and the JSON serialisation is one call.
    HOST_ENVIRONMENT_JSON=$(
        SW_VERS_RAW="$sw_vers_raw" \
        SYSCTL_RAW="$sysctl_raw" \
        KEXTSTAT_RAW="$kextstat_raw" \
        IOREG_RAW="$ioreg_raw" \
        MOUNT_RAW="$mount_raw" \
        LAUNCHD_RAW="$launchd_raw" \
        LOG_STAT_RAW="$log_stat_raw" \
        LOG_PATH_VAR="$LOG_PATH" \
        perl -MJSON::PP -e '
            sub parse_sw_vers {
                my %r;
                for my $line (split /\n/, $ENV{SW_VERS_RAW} // "") {
                    if ($line =~ /^([^:]+):\s*(.*)$/) {
                        my ($k, $v) = ($1, $2);
                        $k = lc $k; $k =~ s/\s+/_/g;
                        $r{$k} = $v;
                    }
                }
                return %r ? \%r : undef;
            }
            sub parse_sysctl {
                # sysctl -n with multiple keys prints one value per line, in
                # the order keys were given: hw.model, hw.ncpu, hw.memsize,
                # machdep.cpu.brand_string. arm64 machines DO populate
                # machdep.cpu.brand_string ("Apple M-series"); older Intel
                # hosts populate it with the brand string. Either way the
                # fourth line is the CPU brand if present.
                my @lines = split /\n/, $ENV{SYSCTL_RAW} // "";
                return undef unless @lines;
                return {
                    hw_model  => $lines[0] // "",
                    ncpu      => (defined $lines[1] && $lines[1] =~ /^\d+$/) ? $lines[1] + 0 : undef,
                    memsize   => (defined $lines[2] && $lines[2] =~ /^\d+$/) ? $lines[2] + 0 : undef,
                    cpu_brand => $lines[3] // "",
                };
            }
            sub parse_kextstat {
                # kextstat format: index refs address size wired NAME (VERSION) UUID ...
                # We want NAME + VERSION + index.
                my @out;
                for my $line (split /\n/, $ENV{KEXTSTAT_RAW} // "") {
                    if ($line =~ /^\s*(\d+)\s+\d+\s+\S+\s+\S+\s+\S+\s+(\S+)\s+\(([^)]+)\)/) {
                        push @out, { load_index => $1 + 0, name => $2, version => $3 };
                    }
                }
                return @out ? \@out : [];
            }
            sub parse_mount {
                # macOS mount lines:
                #   /dev/disk3s1 on / (apfs, sealed, local, read-only, journaled)
                # Network mounts:
                #   //user@host/share on /Volumes/foo (smbfs, ...)
                # ZFS pool/dataset mounts:
                #   tank/data on /Volumes/tank (zfs, local)
                my @out;
                for my $line (split /\n/, $ENV{MOUNT_RAW} // "") {
                    if ($line =~ /^(\S+)\s+on\s+(.+?)\s+\(([^,)]+)(?:,\s*(.+))?\)\s*$/) {
                        push @out, {
                            device      => $1,
                            mount_point => $2,
                            fstype      => $3,
                            options     => ($4 // ""),
                        };
                    }
                }
                return @out ? \@out : [];
            }
            sub parse_log_stat {
                my $s = $ENV{LOG_STAT_RAW} // "";
                return undef unless length $s;
                my %r = ( path => $ENV{LOG_PATH_VAR} );
                $r{size_bytes} = $1 + 0 if $s =~ /size=(\d+)/;
                $r{mtime}      = $1      if $s =~ /mtime=([\d\-T:]+)/;
                return \%r;
            }
            my $env = {
                sw_vers         => parse_sw_vers(),
                hardware        => parse_sysctl(),
                kexts           => parse_kextstat(),
                serial_io_nodes => $ENV{IOREG_RAW},
                mounts          => parse_mount(),
                launchd_status  => $ENV{LAUNCHD_RAW},
                log_file        => parse_log_stat(),
            };
            print JSON::PP->new->canonical->encode($env);
        ' 2>/dev/null
    )
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
# Runs `qm guest exec --timeout N <vmid> -- <path> [args]` and prints
# the envelope JSON ({exited, exitcode, out-data, err-data}). Empty
# output on parse failure.
#
# Earlier versions of this function passed `--output-format json` — that
# flag is not supported by `qm guest exec` on PVE versions tested in
# the wild (PVE returns `400 unable to parse option`). The default
# output IS already valid JSON (PVE's CLIFormatter renders single-hash
# results as indented JSON-compatible text), so no flag is needed.
pve_guest_exec_json() {
    local raw
    raw=$(qm guest exec --timeout "$EXEC_TIMEOUT" "$VMID" -- "$@" 2>/dev/null)
    if printf '%s' "$raw" | perl -MJSON::PP -e 'local $/; eval { decode_json(scalar <STDIN>) }; exit($@ ? 1 : 0)' 2>/dev/null; then
        printf '%s' "$raw"
    fi
}

# --- Transport: libvirt ---------------------------------------------------

libvirt_describe() {
    printf 'libvirt (virsh qemu-agent-command); domain=%s' "$VMID"
}

libvirt_preflight() {
    # 1. virsh and perl on PATH.
    for tool in virsh perl base64; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            fail "preflight — '$tool' not found"
            return 1
        fi
    done

    # 2. Connect to the system libvirtd: requires root or libvirt-group
    # membership. `virsh list --all` is a cheap probe that exits non-zero
    # if the socket is unreachable. Honour LIBVIRT_DEFAULT_URI if set.
    if ! virsh list --all >/dev/null 2>&1; then
        fail "preflight — virsh cannot connect to libvirtd (run as root, join the 'libvirt' group, or set LIBVIRT_DEFAULT_URI)"
        return 1
    fi

    # 3. Domain exists.
    if ! virsh dominfo "$VMID" >/dev/null 2>&1; then
        fail "preflight — libvirt domain '$VMID' not found (try: virsh list --all)"
        return 1
    fi

    return 0
}

libvirt_vm_state() {
    local state
    state=$(virsh domstate "$VMID" 2>/dev/null | head -n 1 | tr -d '[:space:]')
    case "$state" in
        running)              printf 'running' ;;
        shutoff|"shut off")   printf 'stopped' ;;
        "")                   printf 'unknown' ;;
        *)                    printf '%s' "$state" ;;
    esac
}

libvirt_config_summary() {
    local xml
    xml=$(virsh dumpxml "$VMID" 2>/dev/null)
    if [ -z "$xml" ]; then
        fail "domain config not retrievable via virsh dumpxml"
        return
    fi
    # Look for the QGA virtio-serial channel that libvirt documents:
    #   <channel type='unix'>
    #     <target type='virtio' name='org.qemu.guest_agent.0'/>
    #   </channel>
    # Without this, the in-guest agent has nothing to talk to and the
    # transport simply doesn't work — same effect as type=isa missing on
    # PVE, even though the underlying reason (virtio vs ISA) differs.
    if printf '%s' "$xml" | grep -q "org.qemu.guest_agent.0"; then
        pass "guest-agent virtio channel present (org.qemu.guest_agent.0)"
    else
        fail "guest-agent virtio channel missing — add <channel><target name='org.qemu.guest_agent.0'/></channel> to the domain XML"
    fi
    # discard / SSD emulation hints (best-effort grep — libvirt's disk XML
    # is structured but a string grep is enough for an evidence check).
    if printf '%s' "$xml" | grep -q "discard='unmap'\|discard=\"unmap\""; then
        pass "disk discard=unmap (TRIM enabled)"
    else
        info "disk discard not enabled (optional — needed for guest-fstrim)"
    fi
    if printf '%s' "$xml" | grep -q "rotation_rate=\"1\"\|rotation_rate='1'"; then
        pass "disk rotation_rate=1 (SSD emulation)"
    else
        info "disk rotation_rate=1 not set (optional — needed for guest-fstrim)"
    fi
}

# libvirt_qga_cmd <command-suffix> [json-args]
# Issues `guest-<command-suffix>` via `virsh qemu-agent-command` and prints
# the unwrapped response body so downstream `json_query` calls work with
# the same `$d->{field}` shape PVE produces. virsh's envelope is
# `{"return": <body>}` or `{"error": ...}`; we strip `return` if present
# and pass error envelopes through unchanged so the freeze-behavioural
# check can still see the "Command not allowed while filesystem is
# frozen" string in $d->{error}->{desc}.
libvirt_qga_cmd() {
    local cmd="$1"; shift
    local frame
    if [ $# -gt 0 ] && [ -n "$1" ]; then
        frame=$(printf '{"execute":"guest-%s","arguments":%s}' "$cmd" "$1")
    else
        frame=$(printf '{"execute":"guest-%s"}' "$cmd")
    fi
    local raw
    raw=$(virsh qemu-agent-command --timeout "$EXEC_TIMEOUT" "$VMID" "$frame" 2>/dev/null)
    [ -z "$raw" ] && return
    printf '%s' "$raw" | perl -MJSON::PP -e '
        local $/;
        my $d = eval { decode_json(scalar <STDIN>) };
        exit 1 if $@ || !defined $d;
        if (ref $d eq "HASH" && exists $d->{return}) {
            print encode_json($d->{return});
        } else {
            print encode_json($d);
        }
    ' 2>/dev/null
}

# libvirt_guest_exec_json <path> [args...]
# Drives `guest-exec` + `guest-exec-status` via `virsh qemu-agent-command`,
# base64-decodes out-data/err-data, returns a PVE-shaped envelope
# ({exited, exitcode, "out-data": <text>, "err-data": <text>}) so the
# downstream check code is transport-agnostic.
#
# Polls exec-status until exited=true, capped at EXEC_TIMEOUT seconds.
# Sleep granularity is 250ms (Perl select() — no `sleep 0.25` portability
# issue with /bin/sleep on older BSDs).
libvirt_guest_exec_json() {
    local path="$1"; shift
    LV_DOMAIN="$VMID" LV_PATH="$path" LV_TIMEOUT="$EXEC_TIMEOUT" LV_ARGS_JSON=$(
        perl -MJSON::PP -e 'print encode_json([@ARGV])' -- "$@"
    ) perl -MJSON::PP -MMIME::Base64 -e '
        my $domain  = $ENV{LV_DOMAIN};
        my $timeout = $ENV{LV_TIMEOUT} + 0;
        my $args    = decode_json($ENV{LV_ARGS_JSON});
        my $exec_req = encode_json({
            execute   => "guest-exec",
            arguments => {
                path             => $ENV{LV_PATH},
                arg              => $args,
                "capture-output" => JSON::PP::true,
            },
        });
        my $exec_raw = `virsh qemu-agent-command --timeout $timeout \Q$domain\E \Q$exec_req\E 2>/dev/null`;
        exit 1 unless length $exec_raw;
        my $exec = eval { decode_json($exec_raw) };
        exit 1 if $@ || !$exec || !$exec->{return} || !defined $exec->{return}->{pid};
        my $pid = $exec->{return}->{pid};

        my $status_req = encode_json({
            execute   => "guest-exec-status",
            arguments => { pid => $pid + 0 },
        });
        my $deadline = time() + $timeout;
        my $status;
        while (time() < $deadline) {
            my $status_raw = `virsh qemu-agent-command --timeout 5 \Q$domain\E \Q$status_req\E 2>/dev/null`;
            if (length $status_raw) {
                $status = eval { decode_json($status_raw) };
                last if $status && $status->{return} && $status->{return}->{exited};
            }
            select(undef, undef, undef, 0.25);
        }
        exit 1 unless $status && $status->{return} && $status->{return}->{exited};

        my $r = $status->{return};
        my $out = defined $r->{"out-data"} ? decode_base64($r->{"out-data"}) : "";
        my $err = defined $r->{"err-data"} ? decode_base64($r->{"err-data"}) : "";
        print encode_json({
            exited       => JSON::PP::true,
            exitcode     => $r->{exitcode} // 0,
            "out-data"   => $out,
            "err-data"   => $err,
            "out-truncated" => $r->{"out-truncated"} ? JSON::PP::true : JSON::PP::false,
            "err-truncated" => $r->{"err-truncated"} ? JSON::PP::true : JSON::PP::false,
        });
    ' 2>/dev/null
}

# --- Shared: QGA over a Unix socket ---------------------------------------
# Used by both the UTM and qga-socket transports. Both talk to a QGA
# serial socket directly (UTM doesn't expose arbitrary QGA via utmctl,
# only `utmctl exec`; raw QEMU users provide a socket path explicitly).
#
# The socket protocol is line-delimited JSON: write one JSON frame
# followed by a newline, read one JSON frame back. Frames are decoded /
# encoded by Perl using JSON::PP + IO::Socket::UNIX (both core modules
# on stock macOS) so behaviour is uniform across BSD/GNU and doesn't
# depend on `nc`/`socat` version quirks.

# _qga_socket_cmd <socket-path> <command-suffix> [json-args]
# Issues a single QGA frame, prints the unwrapped response body so
# downstream `json_query` calls work with the same `$d->{field}` shape
# PVE produces. Error envelopes pass through unchanged so the
# behavioural-freeze check still sees the error desc.
_qga_socket_cmd() {
    local sock="$1" cmd="$2"; shift 2
    local args="${1:-}"
    QGA_SOCK="$sock" QGA_CMD="$cmd" QGA_ARGS="$args" QGA_TIMEOUT="$EXEC_TIMEOUT" perl -MJSON::PP -MIO::Socket::UNIX -e '
        my $sock = IO::Socket::UNIX->new(
            Peer    => $ENV{QGA_SOCK},
            Type    => SOCK_STREAM(),
            Timeout => $ENV{QGA_TIMEOUT} + 0,
        );
        exit 1 unless $sock;
        my $frame;
        if (length $ENV{QGA_ARGS}) {
            my $args = eval { decode_json($ENV{QGA_ARGS}) };
            exit 1 if $@;
            $frame = encode_json({
                execute   => "guest-" . $ENV{QGA_CMD},
                arguments => $args,
            });
        } else {
            $frame = encode_json({ execute => "guest-" . $ENV{QGA_CMD} });
        }
        $sock->autoflush(1);
        print $sock $frame . "\n" or exit 1;
        # Read one response frame. Some QGA implementations buffer; loop
        # until we have valid JSON or the socket closes.
        my $buf = "";
        local $SIG{ALRM} = sub { die "timeout\n" };
        alarm($ENV{QGA_TIMEOUT} + 0);
        my $decoded;
        while (defined(my $line = <$sock>)) {
            $buf .= $line;
            $decoded = eval { decode_json($buf) };
            last if defined $decoded;
        }
        alarm(0);
        close $sock;
        exit 1 unless defined $decoded;
        if (ref $decoded eq "HASH" && exists $decoded->{return}) {
            print encode_json($decoded->{return});
        } else {
            print encode_json($decoded);
        }
    ' 2>/dev/null
}

# _qga_socket_guest_exec_json <socket-path> <path> [args...]
# Drives guest-exec + guest-exec-status via the socket, polls until
# exited=true (250ms granularity, EXEC_TIMEOUT deadline), base64-decodes
# out-data/err-data, returns a PVE-shape-compatible envelope.
_qga_socket_guest_exec_json() {
    local sock="$1" path="$2"; shift 2
    QGA_SOCK="$sock" QGA_PATH="$path" QGA_TIMEOUT="$EXEC_TIMEOUT" QGA_ARGS_JSON=$(
        perl -MJSON::PP -e 'print encode_json([@ARGV])' -- "$@"
    ) perl -MJSON::PP -MMIME::Base64 -MIO::Socket::UNIX -e '
        sub send_frame {
            my ($frame) = @_;
            my $sock = IO::Socket::UNIX->new(
                Peer    => $ENV{QGA_SOCK},
                Type    => SOCK_STREAM(),
                Timeout => 5,
            );
            return undef unless $sock;
            $sock->autoflush(1);
            print $sock $frame . "\n" or return undef;
            my $buf = "";
            my $decoded;
            local $SIG{ALRM} = sub { die "timeout\n" };
            alarm(5);
            eval {
                while (defined(my $line = <$sock>)) {
                    $buf .= $line;
                    $decoded = eval { decode_json($buf) };
                    last if defined $decoded;
                }
            };
            alarm(0);
            close $sock;
            return $decoded;
        }

        my $args = decode_json($ENV{QGA_ARGS_JSON});
        my $exec_resp = send_frame(encode_json({
            execute   => "guest-exec",
            arguments => {
                path             => $ENV{QGA_PATH},
                arg              => $args,
                "capture-output" => JSON::PP::true,
            },
        }));
        exit 1 unless $exec_resp && $exec_resp->{return} && defined $exec_resp->{return}->{pid};
        my $pid = $exec_resp->{return}->{pid};

        my $deadline = time() + ($ENV{QGA_TIMEOUT} + 0);
        my $status;
        while (time() < $deadline) {
            $status = send_frame(encode_json({
                execute   => "guest-exec-status",
                arguments => { pid => $pid + 0 },
            }));
            last if $status && $status->{return} && $status->{return}->{exited};
            select(undef, undef, undef, 0.25);
        }
        exit 1 unless $status && $status->{return} && $status->{return}->{exited};

        my $r = $status->{return};
        my $out = defined $r->{"out-data"} ? decode_base64($r->{"out-data"}) : "";
        my $err = defined $r->{"err-data"} ? decode_base64($r->{"err-data"}) : "";
        print encode_json({
            exited          => JSON::PP::true,
            exitcode        => $r->{exitcode} // 0,
            "out-data"      => $out,
            "err-data"      => $err,
            "out-truncated" => $r->{"out-truncated"} ? JSON::PP::true : JSON::PP::false,
            "err-truncated" => $r->{"err-truncated"} ? JSON::PP::true : JSON::PP::false,
        });
    ' 2>/dev/null
}

# --- Transport: UTM -------------------------------------------------------
# UTM ships utmctl with `list`, `status`, `start`, `stop`, `exec`,
# `ip-address` etc., but no arbitrary-QGA subcommand. utmctl exec ALONE
# would give us in-guest exec only, missing host-driven ping / osinfo /
# freeze-thaw. We instead talk to the QGA Unix socket directly — same
# socket utmctl exec uses under the hood.
#
# Discovery: UTM 4.x stores per-VM config at
#   ~/Library/Containers/com.utmapp.UTM/Data/Documents/<VM Name>.utm/config.plist
# The plist's Serial array lists each emulated serial interface; the one
# we want has Interface == "QemuGuestAgent" with a Unix-socket Path.
# If no QGA serial is configured, we error with the exact GUI steps to
# add one — we do not mutate the .utm bundle.
#
# `--qga-socket PATH` overrides discovery entirely for users whose
# UTM install differs from the default bundle layout (per-machine
# bundles, custom paths, etc.).

UTM_RESOLVED_SOCKET=""

utm_describe() {
    if [ -n "$QGA_SOCKET" ]; then
        printf 'utm (QGA socket via --qga-socket override); identifier=%s; socket=%s' "$VMID" "$QGA_SOCKET"
    else
        printf 'utm (QGA socket discovered from .utm bundle); name=%s' "$VMID"
    fi
}

# Locate the QGA socket path for a named UTM VM. Tries the standard
# bundle location first; if the plist exists but has no QGA serial,
# errors with the configuration steps. Honours --qga-socket override.
utm_resolve_socket() {
    if [ -n "$QGA_SOCKET" ]; then
        UTM_RESOLVED_SOCKET="$QGA_SOCKET"
        return 0
    fi
    local bundle_root="$HOME/Library/Containers/com.utmapp.UTM/Data/Documents"
    local bundle="$bundle_root/$VMID.utm"
    if [ ! -d "$bundle" ]; then
        # Try UUID lookup via utmctl list (some installs use UUIDs as bundle dirnames).
        local from_list
        from_list=$(utmctl list 2>/dev/null | awk -v name="$VMID" '
            $0 ~ name { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-Fa-f-]{36}$/) { print $i; exit } }')
        if [ -n "$from_list" ]; then
            bundle="$bundle_root/$from_list.utm"
        fi
    fi
    if [ ! -f "$bundle/config.plist" ]; then
        fail "UTM bundle not found: $bundle (try --qga-socket PATH to override discovery)"
        return 1
    fi
    local sock
    sock=$(plutil -convert json -o - "$bundle/config.plist" 2>/dev/null | perl -MJSON::PP -e '
        local $/;
        my $cfg = eval { decode_json(scalar <STDIN>) };
        exit 1 if $@ || !$cfg;
        # UTM 4.x layout: top-level "Serial" array; each entry has
        # "Interface" and "Path" (Unix socket) fields. The schema has
        # shifted across point releases; tolerate either capitalisation.
        my $serials = $cfg->{Serial} // $cfg->{serial} // [];
        for my $s (@$serials) {
            my $iface = $s->{Interface} // $s->{interface} // "";
            next unless $iface eq "QemuGuestAgent";
            my $path = $s->{Path} // $s->{path} // "";
            if (length $path) { print $path; exit 0; }
        }
        exit 2;  # plist parsed, no QGA serial configured
    ' 2>/dev/null)
    local rc=$?
    if [ $rc -eq 2 ]; then
        fail "UTM bundle '$bundle' has no QemuGuestAgent serial port configured."
        info "to fix: open the VM in UTM → Edit → Devices → Serial → Add → Interface: QemuGuestAgent; save and restart the VM."
        info "or pass --qga-socket PATH to point at an existing socket directly."
        return 1
    fi
    if [ -z "$sock" ]; then
        fail "Could not parse a QGA socket path from $bundle/config.plist (UTM config schema may have changed; pass --qga-socket PATH)"
        return 1
    fi
    if [ ! -S "$sock" ]; then
        fail "QGA socket '$sock' (from $bundle/config.plist) is not a live Unix socket — is the VM running?"
        return 1
    fi
    UTM_RESOLVED_SOCKET="$sock"
    return 0
}

utm_preflight() {
    for tool in perl plutil; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            fail "preflight — '$tool' not found"
            return 1
        fi
    done
    # `utmctl` is required only when --qga-socket isn't given (we need
    # utmctl status / list to find the VM). With an override, utmctl
    # presence is informational.
    if [ -z "$QGA_SOCKET" ] && ! command -v utmctl >/dev/null 2>&1; then
        fail "preflight — 'utmctl' not found (install UTM, or pass --qga-socket PATH to skip discovery)"
        return 1
    fi
    # Running as root would break socket ownership — UTM sockets live in
    # the desktop user's container directory and are owned by that user.
    if [ "$(id -u)" -eq 0 ]; then
        fail "preflight — must NOT run as root (UTM sockets are owned by the desktop user; rerun without sudo)"
        return 1
    fi
    if ! utm_resolve_socket; then
        return 1
    fi
    return 0
}

utm_vm_state() {
    if [ -n "$QGA_SOCKET" ]; then
        # In override mode there's no utmctl to ask. Probe the socket
        # itself with a ping; if it answers we treat the VM as running.
        if _qga_socket_cmd "$QGA_SOCKET" ping >/dev/null 2>&1; then
            printf 'running'
        else
            printf 'unknown'
        fi
        return
    fi
    local state
    state=$(utmctl status "$VMID" 2>/dev/null | head -n 1 | tr -d '[:space:]')
    case "$state" in
        started|running)              printf 'running' ;;
        stopped|"shut off"|"shutoff") printf 'stopped' ;;
        "")                            printf 'unknown' ;;
        *)                             printf '%s' "$state" ;;
    esac
}

utm_config_summary() {
    if [ -n "$QGA_SOCKET" ]; then
        pass "QGA socket: $QGA_SOCKET (via --qga-socket override)"
        return
    fi
    if [ -z "$UTM_RESOLVED_SOCKET" ]; then
        fail "QGA socket not resolved (preflight discovery failed; see Preflight section above)"
        return
    fi
    pass "QGA serial port discovered in UTM bundle: $UTM_RESOLVED_SOCKET"
    # We can't usefully inspect UTM's disk-config plist for discard/SSD
    # — the schema is fragmented across UTM releases. Surface as INFO
    # rather than a misleading PASS/FAIL.
    info "discard/SSD hints not checked on UTM (config schema not stable)"
}

utm_qga_cmd() {
    _qga_socket_cmd "$UTM_RESOLVED_SOCKET" "$@"
}

utm_guest_exec_json() {
    _qga_socket_guest_exec_json "$UTM_RESOLVED_SOCKET" "$@"
}

# --- Transport: qga-socket -----------------------------------------------
# Generic raw-QEMU / explicit-socket transport. Identifier is purely
# cosmetic — the socket path comes from --qga-socket PATH. Same socket
# I/O as the UTM transport.

qgasocket_describe() {
    printf 'qga-socket (direct Unix socket); identifier=%s; socket=%s' "${VMID:-<unset>}" "$QGA_SOCKET"
}

qgasocket_preflight() {
    if [ -z "$QGA_SOCKET" ]; then
        fail "preflight — --qga-socket PATH is required for transport=qga-socket"
        return 1
    fi
    for tool in perl; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            fail "preflight — '$tool' not found"
            return 1
        fi
    done
    if [ ! -S "$QGA_SOCKET" ]; then
        fail "preflight — '$QGA_SOCKET' is not a Unix socket (is the VM running?)"
        return 1
    fi
    return 0
}

qgasocket_vm_state() {
    if _qga_socket_cmd "$QGA_SOCKET" ping >/dev/null 2>&1; then
        printf 'running'
    else
        printf 'unknown'
    fi
}

qgasocket_config_summary() {
    pass "QGA socket: $QGA_SOCKET"
    info "host-side config check skipped (qga-socket transport — no hypervisor metadata to inspect)"
}

qgasocket_qga_cmd() {
    _qga_socket_cmd "$QGA_SOCKET" "$@"
}

qgasocket_guest_exec_json() {
    _qga_socket_guest_exec_json "$QGA_SOCKET" "$@"
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
    if command -v virsh >/dev/null 2>&1 && virsh dominfo "$VMID" >/dev/null 2>&1; then
        TRANSPORT="libvirt"
        return 0
    fi
    if command -v utmctl >/dev/null 2>&1 && utmctl status "$VMID" >/dev/null 2>&1; then
        TRANSPORT="utm"
        return 0
    fi
    if [ -n "$QGA_SOCKET" ]; then
        TRANSPORT="qga-socket"
        return 0
    fi
    echo "Error: could not auto-detect transport. Pass --transport pve|libvirt|utm|qga-socket explicitly (or --qga-socket PATH for a generic QGA socket)." >&2
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
        libvirt)
            transport_describe=libvirt_describe
            transport_preflight=libvirt_preflight
            transport_vm_state=libvirt_vm_state
            transport_config_summary=libvirt_config_summary
            transport_qga_cmd=libvirt_qga_cmd
            transport_guest_exec_json=libvirt_guest_exec_json
            ;;
        utm)
            transport_describe=utm_describe
            transport_preflight=utm_preflight
            transport_vm_state=utm_vm_state
            transport_config_summary=utm_config_summary
            transport_qga_cmd=utm_qga_cmd
            transport_guest_exec_json=utm_guest_exec_json
            ;;
        qga-socket)
            transport_describe=qgasocket_describe
            transport_preflight=qgasocket_preflight
            transport_vm_state=qgasocket_vm_state
            transport_config_summary=qgasocket_config_summary
            transport_qga_cmd=qgasocket_qga_cmd
            transport_guest_exec_json=qgasocket_guest_exec_json
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

# --- Host Environment (in-VM probes, captured BEFORE freeze) --------------
# Runs before Freeze/Thaw so the captured mount table reflects pre-freeze
# state and isn't blocked by the freeze command allowlist. (sw_vers,
# sysctl, kextstat, etc. aren't allowlisted commands, so issuing them
# during a freeze window would error.)
if [ "$IN_VM" -eq 1 ] && [ "$ENV_CAPTURE" -eq 1 ]; then
    capture_host_environment
fi

# --- Freeze / thaw (multi-cycle) ------------------------------------------
# macOS has no FIFREEZE; freeze is per-FS dispatch (see docs/design/FREEZE_SEMANTICS.md).
# Beyond the command path this verifies:
#  (a) the frozen STATE behaviourally — while frozen, a non-freeze command
#      must be rejected BY CONTENT (PVE's register_command dispatcher exits
#      0 on QGA errors — see Target 4 in docs/research/UPSTREAM_NOTES.md
#      — so we cannot use exit code);
#  (b) state cleanliness across multiple cycles — re-freezing immediately
#      after thaw must work as if the first cycle never ran, catching
#      state-leak bugs the single-cycle check missed;
#  (c) mount-dispatch consistency — the frozen count reported by the
#      agent must be within range of the writable, non-network, non-
#      special mounts observed in the captured mount table.

FREEZE_LOG_TAIL=""
FREEZE_CYCLES_JSON='[]'
LAST_FROZEN_N=""

# record_cycle <cycle> <frozen_n> <thawed_n> <status_ok> <behavioural> <post_thaw> <log_line>
record_cycle() {
    FREEZE_CYCLES_JSON=$(printf '%s' "$FREEZE_CYCLES_JSON" | \
        CYC="$1" FN="$2" TN="$3" SOK="$4" BHV="$5" PT="$6" LL="$7" \
        perl -MJSON::PP -e '
            local $/;
            my $arr = decode_json(scalar <STDIN>);
            push @$arr, {
                cycle             => $ENV{CYC} + 0,
                frozen_n          => length($ENV{FN}) ? $ENV{FN} + 0 : undef,
                thawed_n          => length($ENV{TN}) ? $ENV{TN} + 0 : undef,
                fsfreeze_status   => $ENV{SOK},
                behavioural_check => $ENV{BHV},
                post_thaw_check   => $ENV{PT},
                freeze_log_line   => $ENV{LL},
            };
            print encode_json($arr);
        ' 2>/dev/null || printf '%s' "$FREEZE_CYCLES_JSON")
}

run_freeze_cycle() {
    local n="$1"
    local frozen_n="" thawed_n="" status_ok="fail" behavioural="ambiguous" post_thaw="fail" log_line=""

    local freeze_resp frozen_n_raw
    freeze_resp=$("$transport_qga_cmd" fsfreeze-freeze 2>/dev/null)
    frozen_n_raw=$(echo "$freeze_resp" | grep -oE '[0-9]+' | head -1)
    if [ -n "$frozen_n_raw" ] && [ "$frozen_n_raw" -ge 1 ]; then
        frozen_n="$frozen_n_raw"
        FROZE=1                                  # arm the auto-thaw trap
        pass "cycle $n: fsfreeze-freeze — $frozen_n filesystem(s) frozen"

        local status_resp
        status_resp=$("$transport_qga_cmd" fsfreeze-status 2>/dev/null)
        if echo "$status_resp" | grep -qw "frozen"; then
            status_ok="pass"
            pass "cycle $n: fsfreeze-status — reports frozen"
        else
            fail "cycle $n: fsfreeze-status — not reported frozen after freeze"
        fi

        local frozen_resp
        frozen_resp=$("$transport_qga_cmd" get-osinfo 2>&1)
        if echo "$frozen_resp" | grep -qE '"pretty-name"|pretty-name:'; then
            behavioural="fail"
            fail "cycle $n: frozen state — agent served get-osinfo while frozen (NOT genuinely gated)"
        elif echo "$frozen_resp" | grep -qiE 'Command not allowed while filesystem is frozen|"error"'; then
            behavioural="pass"
            pass "cycle $n: frozen state — non-freeze command rejected by content"
        else
            info "cycle $n: frozen state — ambiguous response: $(printf '%s' "$frozen_resp" | head -c 200)"
        fi

        local thaw_resp thawed_n_raw
        thaw_resp=$("$transport_qga_cmd" fsfreeze-thaw 2>/dev/null)
        thawed_n_raw=$(echo "$thaw_resp" | grep -oE '[0-9]+' | head -1)
        if [ -n "$thawed_n_raw" ] && [ "$thawed_n_raw" -ge 1 ]; then
            thawed_n="$thawed_n_raw"
            FROZE=""                              # disarm the trap
            pass "cycle $n: fsfreeze-thaw — $thawed_n filesystem(s) thawed"
        else
            fail "cycle $n: fsfreeze-thaw — thaw not confirmed; the VM filesystem may still be frozen"
        fi

        local post_resp
        post_resp=$("$transport_qga_cmd" get-osinfo 2>&1)
        if echo "$post_resp" | grep -qE '"pretty-name"|pretty-name:'; then
            post_thaw="pass"
            pass "cycle $n: post-thaw — agent answers get-osinfo normally again"
        else
            fail "cycle $n: post-thaw — get-osinfo response did not carry pretty-name after thaw"
        fi

        # Per-event freeze INFO line from the agent log (Phase 2 Q3
        # surface). For multi-cycle runs the log carries one INFO line
        # per cycle; we want THIS cycle's line — take the N-th-from-last
        # matching line.
        if [ "$IN_VM" -eq 1 ]; then
            local log_raw log_text
            log_raw=$("$transport_guest_exec_json" /usr/bin/tail -n 500 "$LOG_PATH")
            if [ -n "$log_raw" ]; then
                log_text=$(json_query "$log_raw" '$d->{"out-data"} // ""')
                # Take the LAST matching line — it's the one this cycle
                # just wrote. Earlier cycles' lines are also present but
                # ordered before it.
                log_line=$(printf '%s' "$log_text" | grep -E 'Filesystem frozen:' | tail -n 1)
                if [ -n "$log_line" ]; then
                    pass "cycle $n: freeze log — $log_line"
                else
                    info "cycle $n: freeze log — no 'Filesystem frozen:' INFO line in last 500 lines"
                fi
            fi
        fi

        LAST_FROZEN_N="$frozen_n"
        FREEZE_LOG_TAIL="$log_line"               # last cycle's line for the appendix top-level field
    elif [ -n "$frozen_n_raw" ]; then
        fail "cycle $n: fsfreeze-freeze — froze 0 filesystems (freeze had no effect)"
    else
        fail "cycle $n: fsfreeze-freeze — no response"
    fi

    record_cycle "$n" "$frozen_n" "$thawed_n" "$status_ok" "$behavioural" "$post_thaw" "$log_line"
}

MOUNT_DISPATCH_CROSSCHECK_JSON='null'
if [ "$RUN_FREEZE" -eq 1 ]; then
    section "Freeze / Thaw ($FREEZE_CYCLES cycle(s))"
    for cyc in $(seq 1 "$FREEZE_CYCLES"); do
        run_freeze_cycle "$cyc"
    done

    # Mount-dispatch cross-check: the frozen count from the LAST cycle
    # should be in a reasonable range relative to the count of writable,
    # non-network, non-special mounts captured in the Host Environment
    # section. Loose tolerance (1 to 2x expected) — APFS containers can
    # produce more snapshot rows than mount rows because one container
    # snapshot covers multiple mount points, and ZFS datasets can pad
    # the count too. Strict equality would false-positive.
    section "Mount-Dispatch Cross-Check"
    if [ -z "$HOST_ENVIRONMENT_JSON" ]; then
        info "skipped — no captured mount table (env-capture was off or failed)"
        MOUNT_DISPATCH_CROSSCHECK_JSON='{"skipped":true,"reason":"no captured mount table"}'
    elif [ -z "$LAST_FROZEN_N" ]; then
        info "skipped — no successful freeze cycle to cross-check against"
        MOUNT_DISPATCH_CROSSCHECK_JSON='{"skipped":true,"reason":"no successful freeze cycle"}'
    else
        EXPECTED_N=$(HE="$HOST_ENVIRONMENT_JSON" perl -MJSON::PP -e '
            my $he = decode_json($ENV{HE});
            my %skip_net     = map { $_ => 1 } qw(smbfs afpfs nfs webdav ftp);
            my %skip_special = map { $_ => 1 } qw(devfs autofs fdesc volfs synthfs lifs);
            my $n = 0;
            for my $m (@{$he->{mounts} // []}) {
                my $fs = lc($m->{fstype} // "");
                next if $skip_net{$fs} || $skip_special{$fs};
                # Treat read-only mounts as freeze-relevant — they still get
                # counted under skipped_readonly which contributes to the
                # wire response. Same for any unknown writable type.
                $n++;
            }
            print $n;
        ' 2>/dev/null)
        if [ -z "$EXPECTED_N" ] || [ "$EXPECTED_N" -eq 0 ]; then
            info "expected count not derivable from mount table; freeze reported $LAST_FROZEN_N"
            MOUNT_DISPATCH_CROSSCHECK_JSON=$(printf '{"expected_freeze_n":%s,"actual_freeze_n":%s,"match":null,"reason":"expected count not derivable"}' "${EXPECTED_N:-0}" "$LAST_FROZEN_N")
        else
            UPPER=$((EXPECTED_N * 2))
            if [ "$LAST_FROZEN_N" -ge 1 ] && [ "$LAST_FROZEN_N" -le "$UPPER" ]; then
                pass "mount-dispatch cross-check — $LAST_FROZEN_N mount(s) frozen vs ~$EXPECTED_N expected (within 1..2x range)"
                MOUNT_DISPATCH_CROSSCHECK_JSON=$(printf '{"expected_freeze_n":%s,"actual_freeze_n":%s,"upper_bound":%s,"match":true}' "$EXPECTED_N" "$LAST_FROZEN_N" "$UPPER")
            else
                fail "mount-dispatch cross-check — $LAST_FROZEN_N mount(s) frozen vs ~$EXPECTED_N expected (expected 1..$UPPER)"
                MOUNT_DISPATCH_CROSSCHECK_JSON=$(printf '{"expected_freeze_n":%s,"actual_freeze_n":%s,"upper_bound":%s,"match":false}' "$EXPECTED_N" "$LAST_FROZEN_N" "$UPPER")
            fi
        fi
    fi
else
    section "Freeze / Thaw"
    info "skipped (--no-freeze)"
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
            # The agent's --safe-test-json emits {agent_version, passes,
            # failures, status, test} at the top level (verified against
            # a real El Cap run, May 2026). Accept the synthetic
            # `summary.passed/failed/total` shape as a fallback for any
            # legacy fixture / shim that still emits the older form.
            SA_PASS=$(json_query "$SAFETEST_JSON" '$d->{passes}   // $d->{summary}->{passed} // ""')
            SA_FAIL=$(json_query "$SAFETEST_JSON" '$d->{failures} // $d->{summary}->{failed} // ""')
            SA_STATUS=$(json_query "$SAFETEST_JSON" '$d->{status} // ""')
            if [ -n "$SA_PASS" ] && [ -n "$SA_FAIL" ]; then
                SA_TOTAL=$((SA_PASS + SA_FAIL))
                if [ "$SA_FAIL" -eq 0 ] 2>/dev/null; then
                    pass "--safe-test-json — $SA_PASS/$SA_TOTAL read-only commands passed (status=$SA_STATUS)"
                else
                    fail "--safe-test-json — $SA_FAIL/$SA_TOTAL read-only commands failed (status=$SA_STATUS)"
                fi
            else
                fail "--safe-test-json — could not parse pass/fail counts (got keys: $(json_query "$SAFETEST_JSON" 'join(",", sort keys %$d)'))"
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
