#!/bin/bash
# Exhaustive CLI-verb coverage for mac-guest-agent — runs on the PVE host.
# ======================================================================
# Exercises every command-line verb/flag (non-destructively). ALL cases run in a
# SINGLE guest-exec via an in-guest runner script, so the fragile Tiger ISA
# channel sees one exec, not 21 (21 back-to-back execs wedge it). Output is
# parsed and asserted host-side. Destructive verbs (--install/--upgrade/
# --uninstall without --dry-run) are covered by the lifecycle matrix, not here.
#
# Usage: exhaustive-cli.sh <vmid> [agent_path]
set -u
VMID="${1:?usage: $0 <vmid> [agent_path]}"
AGENT="${2:-/usr/local/bin/mac-guest-agent}"
SOCK="/var/run/qemu-server/${VMID}.qga"

q() { local o; for _ in 1 2 3 4; do o=$( (echo "$1"; sleep 2) | timeout 15 socat - UNIX-CONNECT:"$SOCK" 2>/dev/null); case "$o" in *'"return"'*|*'"error"'*) echo "$o"; return 0;; esac; sleep 2; done; echo "$o"; return 1; }

write_guest_file() { # $1=path $2=content
  local r h b64
  r=$(q "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"$1\",\"mode\":\"w\"}}")
  h=$(echo "$r" | sed -n 's/.*"return":\([0-9]*\).*/\1/p')
  [ -z "$h" ] && { echo "open $1 failed"; return 1; }
  b64=$(printf '%s' "$2" | base64 | tr -d '\n')
  # base64 may exceed the agent's 4096-byte inbound limit -> chunk by 1KB raw.
  local tmp; tmp=$(mktemp); printf '%s' "$2" > "$tmp"
  split -b 1024 "$tmp" "${tmp}.c"
  for f in "${tmp}.c"*; do
    local cb; cb=$(base64 -w0 "$f" 2>/dev/null || base64 "$f" | tr -d '\n')
    q "{\"execute\":\"guest-file-write\",\"arguments\":{\"handle\":$h,\"buf-b64\":\"$cb\"}}" >/dev/null
  done
  q "{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":$h}}" >/dev/null
  rm -f "$tmp" "${tmp}.c"*
}

# --test input files
write_guest_file /private/var/tmp/.cli-ping       '{"execute":"guest-ping"}'
write_guest_file /private/var/tmp/.cli-exec-ping   '{"execute":"guest-exec","arguments":{"path":"/x"}}
{"execute":"guest-ping"}'
write_guest_file /private/var/tmp/.cli-ping-osinfo '{"execute":"guest-ping"}
{"execute":"guest-get-osinfo"}'

# In-guest runner: runs each CLI case, emits a parseable record per case:
#   <<<NAME>>>\n<output>\n<<<EXIT:n>>>
RUNNER='#!/bin/sh
A='"$AGENT"'
emit() { echo "<<<$1>>>"; sh -c "$2" 2>&1; echo "<<<EXIT:$?>>>"; }
emit version      "$A --version"
emit help         "$A --help"
emit help_verbs   "$A --help | grep -cE -- \"--(install|uninstall|upgrade|version|dry-run)\""
emit dumpconf     "$A --dump-conf"
emit selftest     "$A --self-test"
emit selftest_ver "$A --self-test | grep -i \"macOS version\""
emit selftest_cmds "$A --self-test | grep -i \"commands:\""
emit selftest_json "$A --self-test-json | head -c1"
emit safetest     "$A --safe-test"
emit safetest_json "$A --safe-test-json | head -c1"
emit block_rpcs   "$A --test --block-rpcs guest-exec < /private/var/tmp/.cli-exec-ping"
emit allow_rpcs   "$A --test --allow-rpcs guest-ping < /private/var/tmp/.cli-ping-osinfo"
emit test_ping    "$A --test < /private/var/tmp/.cli-ping"
emit install_dry  "$A --install --dry-run"
emit uninstall_dry "$A --uninstall --dry-run"
emit dash_virtio  "$A -virtio"
emit virtio_both  "$A --install --virtio --virtio-force"
emit virtio_noinstall "$A --virtio"
emit upgrade_install "$A --upgrade --install"
emit virtio_refuse "$A --install --virtio --dry-run"
emit unknown_flag "$A --bogus-flag-xyz"
echo "<<<DONE>>>"'
write_guest_file /private/var/tmp/.cli-runner "$RUNNER"

# Run the whole runner in ONE exec; capture to a guest file, cat it back.
RUN=$(q "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\",\"arg\":[\"/private/var/tmp/.cli-runner\"],\"capture-output\":true}}")
PID=$(echo "$RUN" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')
[ -z "$PID" ] && { echo "FATAL: exec failed: $RUN"; exit 2; }
STAT=""
for _ in $(seq 1 30); do sleep 3; STAT=$(q "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":$PID}}"); echo "$STAT" | grep -q '"exited":true' && break; done
echo "$STAT" | python3 -c 'import json,sys,base64
d=json.load(sys.stdin).get("return",{})
print(base64.b64decode(d.get("out-data","")).decode(errors="replace"))' > "/tmp/cli-out-${VMID}.txt"

# Assert host-side. (name, want-substring, want-exit|any)
python3 - "$VMID" "/tmp/cli-out-${VMID}.txt" <<'PYEOF'
import sys,re
vmid,outf=sys.argv[1],sys.argv[2]
text=open(outf).read()
# parse <<<NAME>>> ... <<<EXIT:n>>> blocks
blocks={}
for m in re.finditer(r"<<<([A-Za-z_]+)>>>\n(.*?)<<<EXIT:(\d+)>>>", text, re.S):
    blocks[m.group(1)]=(m.group(2), int(m.group(3)))
CASES=[
 ("version","mac-guest-agent",0),("help","Usage",0),("help_verbs",None,0),
 ("dumpconf","[general]",0),("selftest","self-test",0),("selftest_ver","macOS version",0),
 ("selftest_cmds","42 registered",0),("selftest_json","{",0),("safetest","passed",0),
 ("safetest_json","{",0),("block_rpcs","CommandNotFound",0),("allow_rpcs","CommandNotFound",0),
 ("test_ping","{}",0),("install_dry","DRY RUN",0),("uninstall_dry","DRY RUN",0),
 ("dash_virtio","TWO dashes",1),("virtio_both","cannot combine",1),
 ("virtio_noinstall","modifier for --install",1),("upgrade_install","cannot combine",1),
 ("virtio_refuse",None,None),("unknown_flag",None,None),
]
P=F=0; fails=[]
for name,want,wexit in CASES:
    if name not in blocks: F+=1; fails.append((name,"NO OUTPUT BLOCK (channel wedge?)")); continue
    body,rc=blocks[name]; ok=True; why=""
    if want is not None and want not in body: ok=False; why=f"missing '{want}'"
    if wexit is not None and rc!=wexit: ok=False; why=f"{why}; exit {rc}!={wexit}"
    if ok: P+=1
    else: F+=1; fails.append((name,why))
done="<<<DONE>>>" in text
print(f"=== VM {vmid} CLI: {P} passed, {F} failed (of {len(CASES)}; runner_complete={done}) ===")
for n,w in fails: print(f"  FAIL {n}: {w}")
sys.exit(1 if F else 0)
PYEOF
