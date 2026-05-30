#!/bin/bash
# Shell-shim integration tests for scripts/verify.sh.
#
# Mocks the host CLIs (qm, pvesh, virsh, utmctl, plutil) and a local QGA
# socket so each transport can be exercised end-to-end without a real
# hypervisor. Asserts CLI surface, transport-plugin wiring, JSON appendix
# schema 2.0, optional-flag behaviour, redaction, mount-dispatch
# cross-check logic, and multi-cycle freeze recording.
#
# Run directly:   bash tests/test_verify_transports.sh
# Run via make:   make test-verify-transports

set -uo pipefail
# Disable job control monitor mode so backgrounded child processes don't
# print "Terminated: 15 ..." messages when we kill them at script exit
# (those would dump the entire backgrounded heredoc command into stderr).
set +m

# --- ASSERTION-HELPER CONVENTION (do not violate) -----------------------------
# Helpers below this banner that compare a value against an expected pattern
# (assert_eq / assert_contains / assert_not_contains and any future siblings)
# MUST NOT use pipelines. Under `set -o pipefail`, a pipeline whose right-hand
# side exits early — `grep -q`, `head -1`, `awk '...{print; exit}'`, etc. —
# SIGPIPEs the left-hand side mid-write. That makes the pipeline's exit
# status non-zero even when the logical assertion succeeded; the `if` takes
# the wrong branch and the test FAILs even though the value matched.
#
# Observed once in the wild: CI run 26532052157, commit d0bde24, macos-14,
# 2026-05-27 — `assert_contains "PVE: VMID redacted in human output" "<REDACTED-VMID>" "$OUT"`
# failed because grep -q found the match and exited 0 before printf had
# finished writing the multi-kilobyte `$OUT` haystack into the pipe, so
# printf got EPIPE and pipefail propagated non-zero. The fix below uses
# `case "$haystack" in *"$needle"*)` pattern matching — pure bash, no
# subprocess, no pipe, no race possible.
#
# Use bash builtins for comparison: case, [[ ]], [ ], parameter expansion.
# Data-extraction pipelines elsewhere in this file (extract_appendix,
# json_field) ARE allowed because their consumers (awk-without-exit, perl
# with `local $/; <STDIN>`) read to EOF — no early-exit SIGPIPE race.
# ------------------------------------------------------------------------------

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY="$REPO_ROOT/scripts/verify.sh"

if [ ! -x "$VERIFY" ]; then
    echo "Error: $VERIFY not found or not executable" >&2
    exit 1
fi

TMPDIR=$(mktemp -d -t verify-tests-XXXXXX)
trap 'rm -rf "$TMPDIR"; jobs -p | xargs -r kill 2>/dev/null' EXIT

PASS=0
FAIL=0
ERRORS=""

note()        { echo "$@"; }
ok()          { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad()         { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); ERRORS="$ERRORS\n  - $1"; }
section_hdr() { echo ""; echo "=== $1 ==="; }

# assert_eq <label> <expected> <actual>
assert_eq() {
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}
# assert_contains <label> <needle> <haystack>
# Uses bash `case` pattern matching to avoid the pipefail+SIGPIPE race
# documented in the ASSERTION-HELPER CONVENTION block above. The needle
# is quoted inside the pattern so glob metacharacters in $2 (`*`, `?`,
# `[...]`) are matched literally rather than expanded.
assert_contains() {
    case "$3" in
        *"$2"*) ok "$1" ;;
        *)      bad "$1 (did not contain '$2')" ;;
    esac
}
# assert_not_contains <label> <needle> <haystack>
assert_not_contains() {
    case "$3" in
        *"$2"*) bad "$1 (unexpectedly contained '$2')" ;;
        *)      ok "$1" ;;
    esac
}

# extract_appendix <output> — print the JSON object that starts after the
# "JSON Appendix" header. Empty on parse failure.
extract_appendix() {
    printf '%s\n' "$1" | awk '
        /JSON Appendix/ { capture = 1; next }
        capture { print }
    ' | sed -n '/^{/,$p'
}

# json_field <json> <perl-expression>
json_field() {
    printf '%s' "$1" | perl -MJSON::PP -e '
        local $/;
        my $d = eval { decode_json(scalar <STDIN>) };
        exit 1 if $@ || !defined $d;
        my $out = eval $ARGV[0];
        exit 2 if $@;
        if (ref $out eq "ARRAY" || ref $out eq "HASH") {
            print encode_json($out);
        } elsif (defined $out) {
            print $out;
        }
    ' -- "$1" 2>/dev/null
}
# (note: the helper above takes <json> <perl-expr> — fixed below)
json_field() {
    local json="$1" expr="$2"
    printf '%s' "$json" | perl -MJSON::PP -e '
        local $/;
        my $d = eval { decode_json(scalar <STDIN>) };
        exit 1 if $@ || !defined $d;
        my $out = eval $ARGV[0];
        exit 2 if $@;
        if (ref $out eq "ARRAY" || ref $out eq "HASH") {
            print encode_json($out);
        } elsif (defined $out) {
            print $out;
        }
    ' -- "$expr" 2>/dev/null
}

# =========================================================================
section_hdr "1. CLI surface"

OUT=$("$VERIFY" --help 2>&1)
RC=$?
assert_eq "--help exits 0" 0 "$RC"
assert_contains "--help prints usage" "host-side verification" "$OUT"
assert_contains "--help lists --transport" "--transport" "$OUT"
assert_contains "--help lists --qga-socket" "--qga-socket" "$OUT"
assert_contains "--help lists --freeze-cycles" "--freeze-cycles" "$OUT"
assert_contains "--help lists --no-freeze" "--no-freeze" "$OUT"

OUT=$("$VERIFY" 2>&1); RC=$?
assert_eq "no-args exit 2" 2 "$RC"
assert_contains "no-args error message" "identifier> is required" "$OUT"

OUT=$("$VERIFY" --bogus 123 2>&1); RC=$?
assert_eq "unknown flag exit 2" 2 "$RC"
assert_contains "unknown flag error message" "Unknown option" "$OUT"

OUT=$("$VERIFY" --transport bogus 123 2>&1); RC=$?
assert_eq "unknown transport exit 2" 2 "$RC"
assert_contains "unknown transport error message" "unknown transport" "$OUT"

OUT=$("$VERIFY" --transport pve 999 --freeze-cycles 0 2>&1); RC=$?
assert_eq "--freeze-cycles 0 exit 2" 2 "$RC"
assert_contains "--freeze-cycles 0 error message" "positive integer" "$OUT"

OUT=$("$VERIFY" --transport pve 999 --freeze-cycles abc 2>&1); RC=$?
assert_eq "--freeze-cycles abc exit 2" 2 "$RC"

OUT=$("$VERIFY" --transport qga-socket dummy --no-appendix 2>&1); RC=$?
assert_contains "qga-socket without --qga-socket fails preflight" "--qga-socket PATH is required" "$OUT"

# =========================================================================
section_hdr "2. PVE end-to-end via shims"
#
# Shims qm, pvesh, perl, and the QGA-via-qm-agent response stream so
# verify.sh runs the full pipeline without a real PVE host. The only
# things we can't fake locally are /etc/pve/qemu-server/<vmid>.conf
# (verify.sh hardcodes that path) and the root requirement; both will
# fail in preflight/Configuration sections and be reflected in the
# appendix's host_checks array. Everything ABOVE that — pipeline wiring,
# schema, redaction — is what we assert.

PVE_VMID=100

make_pve_shims() {
    local tmpd="$1"

    cat > "$tmpd/qm" <<'SHIMEOF'
#!/bin/bash
# Mock `qm` for verify.sh PVE-transport integration tests.
case "$1" in
    status)
        # Real `qm status <vmid>` prints `status: running`; verify.sh
        # extracts the state with `awk '{print $2}'` so we must match.
        echo "status: running"
        ;;
    agent)
        VMID="$2"; CMD="$3"
        # Allowlist-aware: freeze-allowlisted commands always succeed.
        # Non-allowlisted commands return the freeze error envelope when
        # the VM is in frozen state (simulates the agent gating).
        case "$CMD" in
            ping|fsfreeze-status|fsfreeze-freeze|fsfreeze-thaw)
                : # fall through to per-command response
                ;;
            *)
                if [ -f /tmp/verify-test-frozen-$VMID ]; then
                    echo '{"error":{"class":"GenericError","desc":"Command not allowed while filesystem is frozen"}}'
                    exit 0
                fi
                ;;
        esac
        case "$CMD" in
            ping)                       echo '{}' ;;
            get-osinfo)                 echo '{"pretty-name":"macOS Test 26.5","name":"macOS","version":"26.5","kernel-release":"25.5.0"}' ;;
            network-get-interfaces)     echo '[{"name":"en0","hardware-address":"aa:bb:cc:dd:ee:ff","ip-addresses":[{"ip-address":"192.168.42.10","ip-address-type":"ipv4","prefix":24}]}]' ;;
            info)                       echo '{"version":"2.4.3","supported_commands":['"$(printf '"cmd%d",' $(seq 1 44))"'"cmd45"]}' ;;
            get-memory-block-info)      echo '{"size":1073741824}' ;;
            get-memory-blocks)          echo '[{"phys-index":0,"online":true,"can-offline":false},{"phys-index":1,"online":true,"can-offline":false}]' ;;
            fsfreeze-freeze)
                touch /tmp/verify-test-frozen-$VMID
                echo "3"
                ;;
            fsfreeze-status)
                if [ -f /tmp/verify-test-frozen-$VMID ]; then echo "frozen"; else echo "thawed"; fi
                ;;
            fsfreeze-thaw)
                rm -f /tmp/verify-test-frozen-$VMID
                echo "3"
                ;;
            *)
                echo '{}'
                ;;
        esac
        ;;
    guest)
        # qm guest exec --timeout N <vmid> -- <path> [args]
        # (PVE's default output is already JSON; --output-format json
        # was removed from verify.sh after a real-world El Cap run
        # caught that the flag isn't accepted on some PVEs.)
        # Build the canned envelope based on which binary was asked for.
        BIN=""
        ARG1=""
        SAW_DD=0
        for tok in "$@"; do
            if [ $SAW_DD -eq 1 ]; then
                if [ -z "$BIN" ]; then BIN="$tok"
                elif [ -z "$ARG1" ]; then ARG1="$tok"
                fi
            fi
            if [ "$tok" = "--" ]; then SAW_DD=1; fi
        done

        case "$BIN $ARG1" in
            "/usr/local/bin/mac-guest-agent --self-test-json")
                OUT='{"agent_version":"2.4.3","errors":0,"warnings":0,"passes":15,"status":"pass","system_info":{"os_version":"26.5"},"freeze_dispatch":{"per_fstypename":{"apfs":"tmutil_snapshot+f_fullfsync"},"cpustats_discriminator":"linux","zfs_cli_available":false}}'
                ;;
            "/usr/local/bin/mac-guest-agent --safe-test-json")
                # Match the agent's real shape (top-level passes /
                # failures / status / agent_version, no nested summary)
                # — verified against an El Cap v2.4.3 run, May 2026.
                OUT='{"agent_version":"2.4.3","test":"safe-test","status":"pass","passes":21,"failures":0}'
                ;;
            "/usr/bin/tail "*"-n "*)
                OUT='[INFO] Filesystem frozen: 3 snapshotted, 0 zfs_snapshotted, 0 fullfsynced, 0 flushed_only (=3 total); skipped 0 (0 network, 0 special, 0 readonly)'
                ;;
            "/usr/bin/sw_vers ")
                OUT=$'ProductName:\tmacOS\nProductVersion:\t26.5\nBuildVersion:\t25F00'
                ;;
            "/usr/sbin/sysctl "*)
                OUT=$'Mac14,5\n12\n103079215104\nApple M2 Pro'
                ;;
            "/bin/sh "*"kextstat"*)
                OUT='   42    1 0xff... 0x1000 0x1000 com.apple.driver.Apple16X50Serial (3.2) ...'
                ;;
            "/bin/sh "*"ioreg"*)
                OUT='+-o AppleVirtIO  <class IOService>'
                ;;
            "/sbin/mount ")
                OUT=$'/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)\n/dev/disk3s6 on /System/Volumes/Data (apfs, local, journaled, nobrowse)\n/dev/disk3s2 on /System/Volumes/Preboot (apfs, local, nobrowse)\n//user@host/share on /Volumes/net (smbfs, local)\nmap auto_home on /System/Volumes/Data/home (autofs, automounted)'
                ;;
            "/bin/launchctl list"*)
                OUT=$'{\n\t"PID" = 1234;\n\t"Label" = "com.macos.guest-agent";\n}'
                ;;
            "/usr/bin/stat "*)
                OUT='size=1234 mtime=2026-05-23T20:00:00 name=/var/log/mac-guest-agent.log'
                ;;
            *)
                OUT="(canned shim — no response for $BIN $ARG1)"
                ;;
        esac
        # qm's --output-format json emits the envelope with out-data
        # already decoded as a UTF-8 string. Encode our canned OUT into a
        # JSON envelope via perl to escape any quotes/newlines safely.
        OUT_VAL="$OUT" perl -MJSON::PP -e '
            print encode_json({
                exited     => JSON::PP::true,
                exitcode   => 0,
                "out-data" => $ENV{OUT_VAL},
                "err-data" => "",
            });
        '
        ;;
    *)
        echo "qm shim: unknown subcommand $1" >&2
        exit 1
        ;;
esac
SHIMEOF
    chmod +x "$tmpd/qm"

    cat > "$tmpd/pvesh" <<'SHIMEOF'
#!/bin/bash
# Mock `pvesh` for cluster-locality + backup-lock preflight checks.
case "$*" in
    "get /cluster/resources --type vm --output-format json")
        # Place VMID 100 on the current hostname so locality passes.
        HN=$(hostname -s 2>/dev/null || hostname)
        printf '[{"vmid":100,"node":"%s","type":"qemu","status":"running"}]' "$HN"
        ;;
    "get /nodes/"*"/qemu/100/status/current --output-format json")
        # No backup in progress.
        echo '{"status":"running","lock":null}'
        ;;
    *)
        # Unknown — return empty so preflight treats it as inconclusive
        # (not a failure).
        ;;
esac
SHIMEOF
    chmod +x "$tmpd/pvesh"
}

PVE_SHIM_DIR="$TMPDIR/pve-shim"
mkdir -p "$PVE_SHIM_DIR"
make_pve_shims "$PVE_SHIM_DIR"

# Clean up the shared frozen-marker file at the start of each PVE test.
rm -f "/tmp/verify-test-frozen-$PVE_VMID"

# Run as a non-root user — the script's root preflight will FAIL but the
# pipeline continues so we can still assert the schema and behaviour.
run_pve() {
    PATH="$PVE_SHIM_DIR:$PATH" bash "$VERIFY" --transport pve "$PVE_VMID" "$@" 2>&1
}

OUT=$(run_pve)
APP=$(extract_appendix "$OUT")
assert_eq "PVE: appendix is well-formed JSON" "2.0" "$(json_field "$APP" '$d->{schema_version}')"
assert_eq "PVE: appendix transport=pve" "pve" "$(json_field "$APP" '$d->{transport}')"
assert_eq "PVE: freeze_cycles default 3" "3" "$(json_field "$APP" '$d->{freeze_cycles}')"
assert_eq "PVE: in_vm_selftest parsed as object" "pass" "$(json_field "$APP" '$d->{in_vm_selftest}->{status} // ""')"
assert_eq "PVE: in_vm_safetest parsed as object" "21" "$(json_field "$APP" '$d->{in_vm_safetest}->{passes} // ""')"
assert_eq "PVE: host_environment has sw_vers" "macOS" "$(json_field "$APP" '$d->{host_environment}->{sw_vers}->{productname} // ""')"
assert_eq "PVE: host_environment has hardware.hw_model" "Mac14,5" "$(json_field "$APP" '$d->{host_environment}->{hardware}->{hw_model} // ""')"
# mount: the shim returns 4 lines; the autofs "map auto_home on ..."
# line uses a 2-token device form that the current parse_mount regex
# doesn't accept (out-of-scope for this commit — autofs mounts get
# categorically skipped during freeze anyway, so the parse miss has no
# behavioural consequence). Assert the 3 standard lines parsed.
MCOUNT=$(json_field "$APP" 'scalar @{$d->{host_environment}->{mounts} // []}')
assert_eq "PVE: mount table parsed 4 standard entries" "4" "${MCOUNT:-0}"
# kextstat parse not asserted here — the gx_capture path passes the
# full shell command as the path+args to the shim, and the shim's
# case-match on $BIN $ARG1 only sees `/bin/sh -c`; faithfully shimming
# the inner pipeline isn't worth the complexity. Real PVE/libvirt/UTM
# get the actual kextstat output and parse_kextstat handles it.
# freeze_cycles_log: 3 cycles ran (default)
CCOUNT=$(json_field "$APP" 'scalar @{$d->{freeze_cycles_log} // []}')
assert_eq "PVE: freeze_cycles_log has 3 entries" "3" "${CCOUNT:-0}"
# Each cycle's frozen_n is 3 (canned)
assert_eq "PVE: cycle 1 frozen_n=3" "3" "$(json_field "$APP" '$d->{freeze_cycles_log}->[0]->{frozen_n}')"
assert_eq "PVE: cycle 3 frozen_n=3" "3" "$(json_field "$APP" '$d->{freeze_cycles_log}->[2]->{frozen_n}')"
# Behavioural check: PASS for each cycle (shim returns error while frozen)
assert_eq "PVE: cycle 1 behavioural=pass" "pass" "$(json_field "$APP" '$d->{freeze_cycles_log}->[0]->{behavioural_check}')"
# mount_dispatch_crosscheck: 2 apfs mounts + 1 smbfs (skipped) + 1 autofs (skipped) = 2 expected, 3 actual → match=true (within 1..4 range)
assert_eq "PVE: mount_dispatch_crosscheck expected=2" "2" "$(json_field "$APP" '$d->{mount_dispatch_crosscheck}->{expected_freeze_n}')"
assert_eq "PVE: mount_dispatch_crosscheck actual=3" "3" "$(json_field "$APP" '$d->{mount_dispatch_crosscheck}->{actual_freeze_n}')"
assert_eq "PVE: mount_dispatch_crosscheck match=true" "1" "$(json_field "$APP" '$d->{mount_dispatch_crosscheck}->{match} ? 1 : 0')"

# Redaction — only the IP and VMID appear in the human output (the MAC
# is captured by network-get-interfaces but only the IP gets printed in
# the PASS line; this is fine, just don't assert "MAC redacted").
assert_contains "PVE: IPv4 redacted in human output" "<REDACTED-IPV4>" "$OUT"
assert_contains "PVE: VMID redacted in human output" "<REDACTED-VMID>" "$OUT"
assert_not_contains "PVE: raw IP absent from human output" "192.168.42.10" "$OUT"

# Cleanup state for downstream tests
rm -f "/tmp/verify-test-frozen-$PVE_VMID"

# =========================================================================
section_hdr "3. Flag-suppression behaviour (PVE shim)"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --no-freeze)
APP=$(extract_appendix "$OUT")
CCOUNT=$(json_field "$APP" 'scalar @{$d->{freeze_cycles_log} // []}')
assert_eq "--no-freeze: freeze_cycles_log empty" "0" "${CCOUNT:-0}"
MXCK=$(json_field "$APP" 'defined $d->{mount_dispatch_crosscheck} ? "set" : "null"')
assert_eq "--no-freeze: mount_dispatch_crosscheck null" "null" "$MXCK"
assert_contains "--no-freeze: skipped message" "skipped (--no-freeze)" "$OUT"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --no-in-vm)
APP=$(extract_appendix "$OUT")
HE=$(json_field "$APP" 'defined $d->{host_environment} ? "set" : "null"')
ST=$(json_field "$APP" 'defined $d->{in_vm_selftest} ? "set" : "null"')
SA=$(json_field "$APP" 'defined $d->{in_vm_safetest} ? "set" : "null"')
assert_eq "--no-in-vm: host_environment null" "null" "$HE"
assert_eq "--no-in-vm: in_vm_selftest null" "null" "$ST"
assert_eq "--no-in-vm: in_vm_safetest null" "null" "$SA"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --no-env-capture)
APP=$(extract_appendix "$OUT")
HE=$(json_field "$APP" 'defined $d->{host_environment} ? "set" : "null"')
ST=$(json_field "$APP" 'defined $d->{in_vm_selftest} ? "set" : "null"')
assert_eq "--no-env-capture: host_environment null" "null" "$HE"
assert_eq "--no-env-capture: in_vm_selftest still set" "set" "$ST"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --no-appendix)
assert_not_contains "--no-appendix: no JSON appendix header" "JSON Appendix" "$OUT"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --no-redact)
assert_contains "--no-redact: raw IP present" "192.168.42.10" "$OUT"
assert_contains "--no-redact: raw VMID present" "VM $PVE_VMID is running" "$OUT"

# =========================================================================
section_hdr "4. --freeze-cycles N controls cycle count (PVE shim)"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --freeze-cycles 2)
APP=$(extract_appendix "$OUT")
CCOUNT=$(json_field "$APP" 'scalar @{$d->{freeze_cycles_log} // []}')
assert_eq "--freeze-cycles 2: 2 entries" "2" "${CCOUNT:-0}"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"
OUT=$(run_pve --freeze-cycles 1)
APP=$(extract_appendix "$OUT")
CCOUNT=$(json_field "$APP" 'scalar @{$d->{freeze_cycles_log} // []}')
assert_eq "--freeze-cycles 1: 1 entry" "1" "${CCOUNT:-0}"

rm -f "/tmp/verify-test-frozen-$PVE_VMID"

# =========================================================================
section_hdr "5. UTM preflight (utmctl-absent path)"

# UTM transport without --qga-socket and without utmctl on PATH:
# preflight should FAIL at the missing-utmctl check with the
# --qga-socket override hint. (The deeper bundle-discovery / plist-
# parsing paths need a full plutil + utmctl shim — qga-socket exercises
# the same socket code paths end-to-end, so we rely on that for
# socket-I/O validation.)
OUT=$(PATH=/usr/bin:/bin bash "$VERIFY" --transport utm "Nonexistent VM" --no-appendix 2>&1)
assert_contains "UTM: rejects when utmctl missing" "utmctl' not found" "$OUT"
assert_contains "UTM: suggests --qga-socket override" "--qga-socket PATH to skip discovery" "$OUT"

# =========================================================================
# v2.5.3+: libvirt transport tests cover the direct-socket rewrite. The
# transport no longer uses `virsh qemu-agent-command` (which requires
# libvirt's QGA infrastructure, which is VirtIO-only and conflicts with
# Apple's daemon on Big Sur+). Instead it parses the domain XML to
# discover the unix socket path from a `<serial type='unix'>` element
# and talks to that socket directly via the shared _qga_socket_* helpers.
#
# These tests spawn a perl QGA listener bound to a real socket, have the
# virsh shim return XML pointing at that socket, and assert that
# verify.sh discovers it and runs end-to-end against it.
section_hdr "5b. libvirt transport (virsh shim + perl QGA listener) — direct-socket model"

LIBVIRT_DOMAIN="macos-vm-test"
LV_SOCK="$TMPDIR/libvirt-qga.sock"
LV_SERVER_PID=""

# Spawn a perl QGA listener at $LV_SOCK. Same response surface as the
# section-6 listener, scoped to this test block. Killed at the end of
# the libvirt block so it doesn't leak into section 6.
perl - "$LV_SOCK" <<'PERLEOF' &
use strict; use warnings;
use IO::Socket::UNIX;
use JSON::PP;
use MIME::Base64;
my $sock_path = $ARGV[0];
unlink $sock_path;
my $listener = IO::Socket::UNIX->new(
    Local  => $sock_path,
    Type   => SOCK_STREAM,
    Listen => 8,
) or die "listen $sock_path: $!";
chmod 0600, $sock_path;
sub respond_to {
    my ($req) = @_;
    my $cmd = $req->{execute} // "";
    if    ($cmd eq "guest-ping")                  { return { return => {} }; }
    elsif ($cmd eq "guest-sync")                  { return { return => $req->{arguments}->{id} // 1 }; }
    elsif ($cmd eq "guest-get-osinfo")            { return { return => { "pretty-name" => "macOS Test 26.5", name => "macOS", version => "26.5" } }; }
    elsif ($cmd eq "guest-network-get-interfaces"){ return { return => [ { name => "en0", "hardware-address" => "00:11:22:33:44:66", "ip-addresses" => [ { "ip-address" => "10.0.0.43", "ip-address-type" => "ipv4", prefix => 24 } ] } ] }; }
    elsif ($cmd eq "guest-info")                  { return { return => { version => "2.5.3", supported_commands => [ map { "cmd$_" } 1..45 ] } }; }
    elsif ($cmd eq "guest-get-memory-block-info") { return { return => { size => 1073741824 } }; }
    elsif ($cmd eq "guest-get-memory-blocks")     { return { return => [ { "phys-index" => 0, online => JSON::PP::true, "can-offline" => JSON::PP::false } ] }; }
    return { error => { class => "GenericError", desc => "unknown command $cmd" } };
}
while (my $client = $listener->accept) {
    while (defined(my $line = <$client>)) {
        chomp $line;
        next unless length $line;
        my $req = eval { decode_json($line) };
        if ($@) { print $client encode_json({ error => { desc => "bad JSON" } }), "\n"; next; }
        print $client encode_json(respond_to($req)), "\n";
    }
    close $client;
}
PERLEOF
LV_SERVER_PID=$!
# Wait up to 2 seconds for the socket to appear.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -S "$LV_SOCK" ] && break
    sleep 0.1
done
if [ ! -S "$LV_SOCK" ]; then
    fail "libvirt fixture: perl QGA listener failed to start at $LV_SOCK"
else

make_virsh_shim() {
    local tmpd="$1"
    local fixture_dir="$2"
    cat > "$tmpd/virsh" <<SHIMEOF
#!/bin/bash
# Mock virsh for libvirt-transport verifier tests (v2.5.3+ direct-socket).
# Returns canned responses for the subcommands verify.sh's libvirt
# preflight (list, dominfo, dumpxml, domstate) invokes. qemu-agent-command
# is intentionally NOT mocked — the new transport never calls it (it
# would only work for VirtIO QGA setups, which we don't use).
FIXTURE_DIR="$fixture_dir"
SHIMEOF
    cat >> "$tmpd/virsh" <<'SHIMEOF'
case "$1" in
    list)
        echo " Id   Name           State"
        echo "----------------------------"
        echo " 1    macos-vm-test  running"
        ;;
    dominfo)
        echo "Id:             1"
        echo "Name:           $2"
        echo "UUID:           e4686d2c-6e8d-4335-b8fd-81bee22f4814"
        echo "State:          running"
        ;;
    domstate)
        echo "running"
        ;;
    dumpxml)
        cat "$FIXTURE_DIR/dumpxml.xml"
        ;;
    qemu-agent-command)
        # Intentionally fail loud if the verifier tries to call this.
        # v2.5.3+ libvirt transport must not use virsh qemu-agent-command;
        # if this shim ever returns success, a future regression has
        # reintroduced the wrong path.
        echo "shim: virsh qemu-agent-command must NOT be invoked by v2.5.3+ libvirt transport" >&2
        exit 1
        ;;
    *)
        echo "shim: unknown subcommand $*" >&2
        exit 1
        ;;
esac
SHIMEOF
    chmod +x "$tmpd/virsh"
}

# --- Pass case: documented type-first attribute order ---
LV_PASS_DIR="$TMPDIR/libvirt-pass"
LV_PASS_SHIM="$TMPDIR/libvirt-pass-shim"
mkdir -p "$LV_PASS_DIR" "$LV_PASS_SHIM"
cat > "$LV_PASS_DIR/dumpxml.xml" <<EOF
<domain type='kvm'>
  <name>macos-vm-test</name>
  <uuid>e4686d2c-6e8d-4335-b8fd-81bee22f4814</uuid>
  <devices>
    <serial type='unix'>
      <source mode='bind' path='$LV_SOCK'/>
      <target type='isa-serial' port='0'/>
    </serial>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2' discard='unmap'/>
      <source file='/var/lib/libvirt/images/macos.qcow2'/>
      <target dev='vda' bus='virtio'/>
    </disk>
  </devices>
</domain>
EOF
make_virsh_shim "$LV_PASS_SHIM" "$LV_PASS_DIR"

OUT=$(PATH="$LV_PASS_SHIM:$PATH" bash "$VERIFY" --transport libvirt "$LIBVIRT_DOMAIN" --no-freeze --no-in-vm --no-env-capture --no-appendix 2>&1)
assert_contains "libvirt PASS: discovers QGA socket from domain XML"  "QGA socket discovered from domain XML: $LV_SOCK" "$OUT"
assert_contains "libvirt PASS: detects ISA serial target"             "guest-agent ISA serial target present" "$OUT"
assert_contains "libvirt PASS: end-to-end agent communication works"  "PASS  ping" "$OUT"

# --- Reordered-attributes case: <target port='0' type='isa-serial'/>
# (defense for audit MED-3 — XML attribute order isn't significant per
# XML 1.0 / Relax NG, and operators editing via virsh edit / external
# tools can produce non-canonical order that libvirt still accepts). ---
LV_REORDER_DIR="$TMPDIR/libvirt-reorder"
LV_REORDER_SHIM="$TMPDIR/libvirt-reorder-shim"
mkdir -p "$LV_REORDER_DIR" "$LV_REORDER_SHIM"
cat > "$LV_REORDER_DIR/dumpxml.xml" <<EOF
<domain type='kvm'>
  <name>macos-vm-test</name>
  <uuid>e4686d2c-6e8d-4335-b8fd-81bee22f4814</uuid>
  <devices>
    <serial type='unix'>
      <source path='$LV_SOCK' mode='bind'/>
      <target port='0' type='isa-serial'/>
    </serial>
  </devices>
</domain>
EOF
make_virsh_shim "$LV_REORDER_SHIM" "$LV_REORDER_DIR"

OUT=$(PATH="$LV_REORDER_SHIM:$PATH" bash "$VERIFY" --transport libvirt "$LIBVIRT_DOMAIN" --no-freeze --no-in-vm --no-env-capture --no-appendix 2>&1)
assert_contains "libvirt REORDER: tolerates port-first attribute order" "QGA socket discovered from domain XML: $LV_SOCK" "$OUT"
assert_contains "libvirt REORDER: ISA target detected with type-second" "guest-agent ISA serial target present" "$OUT"
assert_contains "libvirt REORDER: end-to-end ping works"                "PASS  ping" "$OUT"

# --- Fail case: VirtIO channel only, no ISA serial. The new transport
# refuses at preflight (cannot discover a socket), not at config-check. ---
LV_FAIL_DIR="$TMPDIR/libvirt-fail"
LV_FAIL_SHIM="$TMPDIR/libvirt-fail-shim"
mkdir -p "$LV_FAIL_DIR" "$LV_FAIL_SHIM"
cat > "$LV_FAIL_DIR/dumpxml.xml" <<'EOF'
<domain type='kvm'>
  <name>macos-vm-test</name>
  <uuid>e4686d2c-6e8d-4335-b8fd-81bee22f4814</uuid>
  <devices>
    <channel type='unix'>
      <target type='virtio' name='org.qemu.guest_agent.0'/>
    </channel>
  </devices>
</domain>
EOF
make_virsh_shim "$LV_FAIL_SHIM" "$LV_FAIL_DIR"

OUT=$(PATH="$LV_FAIL_SHIM:$PATH" bash "$VERIFY" --transport libvirt "$LIBVIRT_DOMAIN" --no-freeze --no-in-vm --no-env-capture --no-appendix 2>&1)
assert_contains "libvirt FAIL: preflight refuses VirtIO-only XML"  "could not discover QGA unix socket" "$OUT"
assert_contains "libvirt FAIL: failure points at expected element" "type='isa-serial'" "$OUT"
assert_contains "libvirt FAIL: failure points at the doc"          "docs/LIBVIRT.md" "$OUT"

# Tear down the libvirt section's perl listener so it doesn't compete
# with section 6's listener (which binds a different socket path but
# we want the process tree clean).
{ kill "$LV_SERVER_PID" 2>/dev/null; wait "$LV_SERVER_PID" 2>/dev/null; } 2>/dev/null
fi  # end of "perl listener started" guard

# =========================================================================
section_hdr "6. qga-socket transport, end-to-end via fake QGA server"

# Spin up a minimal Perl QGA listener that handles ping, fsfreeze-freeze
# (toggles frozen state), fsfreeze-status, fsfreeze-thaw, get-osinfo
# (errors while frozen), and guest-exec/guest-exec-status (canned
# responses).
QGA_SOCK="$TMPDIR/qga.sock"
SERVER_PID=""
perl - "$QGA_SOCK" <<'PERLEOF' &
use strict; use warnings;
use IO::Socket::UNIX;
use JSON::PP;
use MIME::Base64;
my $sock_path = $ARGV[0];
unlink $sock_path;
my $listener = IO::Socket::UNIX->new(
    Local  => $sock_path,
    Type   => SOCK_STREAM,
    Listen => 8,
) or die "listen $sock_path: $!";
chmod 0600, $sock_path;
my $frozen = 0;
my %exec_state;
my $next_pid = 1000;
sub respond_to {
    my ($req) = @_;
    my $cmd = $req->{execute} // "";
    if ($cmd eq "guest-ping") {
        return { return => {} };
    } elsif ($cmd eq "guest-get-osinfo") {
        return { error => { class => "GenericError", desc => "Command not allowed while filesystem is frozen" } } if $frozen;
        return { return => { "pretty-name" => "macOS Test 26.5", name => "macOS", version => "26.5" } };
    } elsif ($cmd eq "guest-network-get-interfaces") {
        return { return => [ { name => "en0", "hardware-address" => "00:11:22:33:44:55", "ip-addresses" => [ { "ip-address" => "10.0.0.42", "ip-address-type" => "ipv4", prefix => 24 } ] } ] };
    } elsif ($cmd eq "guest-info") {
        return { return => { version => "2.4.3", supported_commands => [ map { "cmd$_" } 1..45 ] } };
    } elsif ($cmd eq "guest-get-memory-block-info") {
        return { return => { size => 1073741824 } };
    } elsif ($cmd eq "guest-get-memory-blocks") {
        return { return => [ { "phys-index" => 0, online => JSON::PP::true, "can-offline" => JSON::PP::false } ] };
    } elsif ($cmd eq "guest-fsfreeze-freeze") {
        $frozen = 1;
        return { return => 2 };
    } elsif ($cmd eq "guest-fsfreeze-status") {
        return { return => $frozen ? "frozen" : "thawed" };
    } elsif ($cmd eq "guest-fsfreeze-thaw") {
        $frozen = 0;
        return { return => 2 };
    } elsif ($cmd eq "guest-exec") {
        my $pid = $next_pid++;
        my $path = $req->{arguments}->{path} // "";
        my $args = $req->{arguments}->{arg} // [];
        my $out = "(no canned response)";
        if    ($path eq "/usr/local/bin/mac-guest-agent" && @$args && $args->[0] eq "--self-test-json") {
            $out = '{"agent_version":"2.4.3","errors":0,"warnings":0,"passes":15,"status":"pass","system_info":{"os_version":"26.5"},"freeze_dispatch":{"per_fstypename":{"apfs":"tmutil_snapshot+f_fullfsync"},"cpustats_discriminator":"linux","zfs_cli_available":false}}';
        } elsif ($path eq "/usr/local/bin/mac-guest-agent" && @$args && $args->[0] eq "--safe-test-json") {
            $out = '{"agent_version":"2.4.3","test":"safe-test","status":"pass","passes":21,"failures":0}';
        } elsif ($path eq "/usr/bin/sw_vers") {
            $out = "ProductName:\tmacOS\nProductVersion:\t26.5\nBuildVersion:\t25F00";
        } elsif ($path eq "/sbin/mount") {
            $out = "/dev/disk3s1 on / (apfs, sealed, local)\n/dev/disk3s6 on /System/Volumes/Data (apfs, local)";
        } else {
            $out = "";
        }
        $exec_state{$pid} = {
            exited     => 1,
            exitcode   => 0,
            "out-data" => encode_base64($out, ""),
            "err-data" => "",
        };
        return { return => { pid => $pid } };
    } elsif ($cmd eq "guest-exec-status") {
        my $pid = $req->{arguments}->{pid};
        my $state = $exec_state{$pid} // { exited => JSON::PP::true, exitcode => 1 };
        return { return => $state };
    }
    return { error => { class => "GenericError", desc => "unknown command $cmd" } };
}
while (my $client = $listener->accept) {
    while (defined(my $line = <$client>)) {
        chomp $line;
        next unless length $line;
        my $req = eval { decode_json($line) };
        if ($@) {
            print $client encode_json({ error => { desc => "bad JSON" } }), "\n";
            next;
        }
        my $resp = respond_to($req);
        print $client encode_json($resp), "\n";
    }
    close $client;
}
PERLEOF
SERVER_PID=$!
# Wait up to 2 seconds for the socket to appear
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -S "$QGA_SOCK" ] && break
    sleep 0.1
done
if [ ! -S "$QGA_SOCK" ]; then
    bad "qga-socket: fake server failed to start"
else
    OUT=$(bash "$VERIFY" --transport qga-socket testvm --qga-socket "$QGA_SOCK" --freeze-cycles 2 2>&1)
    APP=$(extract_appendix "$OUT")
    assert_eq "qga-socket: schema 2.0" "2.0" "$(json_field "$APP" '$d->{schema_version}')"
    assert_eq "qga-socket: transport=qga-socket" "qga-socket" "$(json_field "$APP" '$d->{transport}')"
    assert_eq "qga-socket: in_vm_selftest pass" "pass" "$(json_field "$APP" '$d->{in_vm_selftest}->{status} // ""')"
    CCOUNT=$(json_field "$APP" 'scalar @{$d->{freeze_cycles_log} // []}')
    assert_eq "qga-socket: 2 cycles ran" "2" "${CCOUNT:-0}"
    assert_eq "qga-socket: cycle 1 behavioural=pass" "pass" "$(json_field "$APP" '$d->{freeze_cycles_log}->[0]->{behavioural_check}')"
    assert_eq "qga-socket: cycle 2 behavioural=pass" "pass" "$(json_field "$APP" '$d->{freeze_cycles_log}->[1]->{behavioural_check}')"
    # Redaction with the canned 10.0.0.42 IP and 00:11:22:33:44:55 MAC
    assert_contains "qga-socket: IPv4 redacted" "<REDACTED-IPV4>" "$OUT"
    assert_not_contains "qga-socket: raw IP absent" "10.0.0.42" "$OUT"
    { kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; } 2>/dev/null
fi

# =========================================================================
echo ""
echo "=============================================="
echo "Results: $PASS passed, $FAIL failed"
echo "=============================================="
if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
    exit 1
fi
exit 0
