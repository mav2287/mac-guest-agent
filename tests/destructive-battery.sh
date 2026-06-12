#!/bin/sh
# Destructive / mutating outcome battery — verifies REAL side effects, not just
# response shape. Stateful command sequences (file handles, exec pids) run in a
# SINGLE `agent --test` session so state persists; in --test the handle counter
# starts at 1000 and the exec pid counter at 1 each session, so they're
# predictable. POSIX sh + openssl base64 → runs Tiger 10.4 through current.
# Excludes shutdown/suspend/fsfreeze (exercised at the OS/daemon level).
set -u
# HOME is unset in the daemon's guest-exec child env (minimal launchd env);
# derive it so the ssh-keys section works whether run via SSH or guest-exec.
: "${HOME:=$(eval echo ~"$(whoami)")}"
AGENT=/usr/local/bin/mac-guest-agent
TMP=/tmp/mga-destruct.$$; mkdir -p "$TMP"
PASS=0; FAIL=0
b64e() { openssl base64 -e -A 2>/dev/null || openssl base64 | tr -d '\n'; }
b64d() { openssl base64 -d -A 2>/dev/null || openssl base64 -d; }
# run a multi-line command stream through ONE --test session; print response lines
session() { printf '%s' "$1" | "$AGENT" --test 2>/dev/null | sed 's/^QMP> //' | grep '^{'; }
nth() { sed -n "${2}p" <<EOF
$1
EOF
}
ok()  { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL: $1  [$2]"; }
numf() { echo "$1" | sed -n "s/.*\"$2\":\([0-9][0-9]*\).*/\1/p" | head -1; }
strf() { echo "$1" | sed -n "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p" | head -1; }

# NOTE: guest-exec output capture is async (returns a pid; output drains over
# subsequent loop ticks) and so is tested over the DAEMON with real poll-delays
# — not here. In --test (synchronous stdin) the child has no wall-clock time to
# exit before exec-status, so it always reads exited:false. This battery covers
# the synchronous, state-bearing commands; exec is covered by daemon-side tests.

echo "=== guest-file: write/read/seek integrity (one session, handles 1000/1001) ==="
F="$TMP/f.bin"; DATA="The quick brown fox 0123456789"; DB64=$(printf '%s' "$DATA" | b64e)
R=$(session '{"execute":"guest-file-open","arguments":{"path":"'"$F"'","mode":"w"}}
{"execute":"guest-file-write","arguments":{"handle":1000,"buf-b64":"'"$DB64"'"}}
{"execute":"guest-file-flush","arguments":{"handle":1000}}
{"execute":"guest-file-close","arguments":{"handle":1000}}
{"execute":"guest-file-open","arguments":{"path":"'"$F"'","mode":"r"}}
{"execute":"guest-file-read","arguments":{"handle":1001,"count":1024}}
{"execute":"guest-file-read","arguments":{"handle":1001,"count":1024}}
{"execute":"guest-file-seek","arguments":{"handle":1001,"offset":4,"whence":0}}
{"execute":"guest-file-read","arguments":{"handle":1001,"count":5}}
{"execute":"guest-file-close","arguments":{"handle":1001}}')
# lines: 1 open(w) 2 write 3 flush 4 close 5 open(r) 6 read-all 7 read-eof 8 seek 9 read-5 10 close
[ "$(numf "$(nth "$R" 1)" return)" = "1000" ] && ok "file-open(w) handle 1000" || bad "file-open w" "got '$(nth "$R" 1)'"
[ "$(numf "$(nth "$R" 2)" count)" = "${#DATA}" ] && ok "file-write count=${#DATA}" || bad "file-write count" "got '$(nth "$R" 2)'"
[ "$(cat "$F")" = "$DATA" ] && ok "file on disk matches written bytes" || bad "file on disk" "got '$(cat "$F" 2>/dev/null)'"
RD=$(strf "$(nth "$R" 6)" buf-b64 | b64d)
[ "$RD" = "$DATA" ] && ok "file-read returns identical bytes" || bad "file-read" "got '$RD'"
echo "$(nth "$R" 7)" | grep -q '"eof":true' && ok "file-read at end -> eof:true (0 bytes)" || bad "file-read eof" "got '$(nth "$R" 7)'"
RD=$(strf "$(nth "$R" 9)" buf-b64 | b64d)
[ "$RD" = "quick" ] && ok "file-seek(4)+read(5)='quick'" || bad "file-seek" "got '$RD'"

echo "=== guest-file error paths ==="
session '{"execute":"guest-file-read","arguments":{"handle":99999,"count":1}}' | grep -q '"error"' && ok "read bad handle -> error" || bad "read bad handle" "no error"
session '{"execute":"guest-file-open","arguments":{"path":"/no/such/dir/x","mode":"r"}}' | grep -q '"error"' && ok "open nonexistent -> error" || bad "open nonexistent" "no error"

echo "=== guest-ssh: add/get/remove authorized-keys (backup+restore) ==="
U=$(whoami); AK="$HOME/.ssh/authorized_keys"; BK="$TMP/ak.bak"; HAD=0
[ -f "$AK" ] && { cp "$AK" "$BK"; HAD=1; }
TK="ssh-rsa AAAATESTKEYmga$$ mga-destruct-test"
session '{"execute":"guest-ssh-add-authorized-keys","arguments":{"username":"'"$U"'","keys":["'"$TK"'"]}}' >/dev/null
session '{"execute":"guest-ssh-get-authorized-keys","arguments":{"username":"'"$U"'"}}' | grep -q "mga-destruct-test" && ok "ssh-add then get shows key" || bad "ssh-add/get" "missing"
session '{"execute":"guest-ssh-remove-authorized-keys","arguments":{"username":"'"$U"'","keys":["'"$TK"'"]}}' >/dev/null
session '{"execute":"guest-ssh-get-authorized-keys","arguments":{"username":"'"$U"'"}}' | grep -q "mga-destruct-test" && bad "ssh-remove" "still present" || ok "ssh-remove deletes key"
if [ "$HAD" = "1" ]; then cp "$BK" "$AK"; else rm -f "$AK"; fi; ok "authorized_keys restored"

echo "=== guest-set-user-password: set to same value (safe round-trip) ==="
session '{"execute":"guest-set-user-password","arguments":{"username":"'"$U"'","password":"'"$(printf password | b64e)"'","crypted":false}}' | grep -q '"return"' && ok "set-user-password (same pw)" || bad "set-user-password" "no return"

rm -rf "$TMP"
echo "DESTRUCTIVE BATTERY: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
