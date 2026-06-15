#!/bin/bash
# Exhaustive command-coverage battery for mac-guest-agent — runs on the PVE host.
# ==============================================================================
# Drives the agent's `--test` mode (stdin/stdout protocol) via ONE guest-exec,
# exercising every registered QGA command plus error paths and edge cases.
# Responses are asserted host-side with jq. --test bypasses the ISA-serial
# channel (no wedge) and needs nothing in-guest but the binary + a POSIX shell.
#
# SAFETY: --test forces only fsfreeze to dry-run. So shutdown/suspend are NEVER
# in this battery (they would really halt the VM); mutating commands appear only
# in non-mutating error forms or against scratch targets.
#
# Usage:  exhaustive-commands.sh <vmid> [agent_path]
# Exit 0 iff every assertion passed.
set -u
VMID="${1:?usage: $0 <vmid> [agent_path]}"
AGENT="${2:-/usr/local/bin/mac-guest-agent}"
SOCK="/var/run/qemu-server/${VMID}.qga"
BATT="/tmp/mga-battery-${VMID}.txt"

# REQUEST @@ ASSERTION   (assert: ok | err | err:Class | jq:<expr-over-.return>)
cat > "$BATT" <<'BATTERY_END'
{"execute":"guest-ping"} @@ jq:. == {}
{"execute":"guest-info"} @@ jq:(.version|type=="string") and (.supported_commands|length>=40)
{"execute":"guest-get-osinfo"} @@ jq:.id=="macos" and (.["kernel-release"]|length>0) and (.name|length>0)
{"execute":"guest-get-host-name"} @@ jq:(.["host-name"]|length)>0
{"execute":"guest-get-hostname"} @@ jq:(.["host-name"]|length)>0
{"execute":"guest-get-timezone"} @@ jq:.offset|type=="number"
{"execute":"guest-get-time"} @@ jq:. > 1600000000000000000
{"execute":"guest-get-users"} @@ jq:type=="array"
{"execute":"guest-get-load"} @@ jq:.load1m>=0 and .load5m>=0 and .load15m>=0
{"execute":"guest-get-vcpus"} @@ jq:(length>=1) and (.[0].online==true) and (.[0]["logical-id"]==0)
{"execute":"guest-get-memory-blocks"} @@ jq:length>=1
{"execute":"guest-get-memory-block-info"} @@ jq:.size>=1048576
{"execute":"guest-get-cpustats"} @@ jq:(length>=1) and (.[0].idle>=0)
{"execute":"guest-get-disks"} @@ jq:(length>=1) and (.[0].name|length>0)
{"execute":"guest-get-diskstats"} @@ jq:length>=1
{"execute":"guest-get-fsinfo"} @@ jq:(map(select(.mountpoint=="/"))|length)==1
{"execute":"guest-fsfreeze-status"} @@ jq:.=="thawed"
{"execute":"guest-network-get-interfaces"} @@ jq:(length>=1) and ([.[]|select(.["hardware-address"]? // ""|length>0)]|length>=1)
{"execute":"guest-network-get-interfaces"} @@ jq:([.[]|select(.name=="lo0")]|length)==1 and ([.[]|select(.name=="lo0")][0]["ip-addresses"]|map(.["ip-address"])|index("127.0.0.1")!=null)
{"execute":"guest-network-get-interfaces"} @@ jq:([.[]|select(.name=="lo0")][0].statistics) as $s | ($s["rx-errs"]==0 and $s["tx-errs"]==0 and (($s["rx-bytes"]==0) or ($s["rx-packets"]>0)))
{"execute":"guest-network-get-route"} @@ jq:length>=1
{"execute":"guest-network-get-route"} @@ jq:([.[]|select(.destination=="127.0.0.0")][0].desprefixlen)=="8"
{"execute":"guest-sync","arguments":{"id":12345}} @@ jq:. == 12345
{"execute":"guest-sync-delimited","arguments":{"id":67890}} @@ jq:. == 67890
{"execute":"guest-fsfreeze-freeze"} @@ ok
{"execute":"guest-fsfreeze-status"} @@ jq:.=="frozen" or .=="thawed"
{"execute":"guest-fsfreeze-thaw"} @@ ok
{"execute":"guest-fsfreeze-freeze-list","arguments":{"mountpoints":["/"]}} @@ ok
{"execute":"guest-fsfreeze-thaw"} @@ ok
{"execute":"guest-fstrim"} @@ err:CommandNotFound
{"execute":"guest-set-vcpus","arguments":{"vcpus":[{"logical-id":0,"online":false}]}} @@ err:CommandNotFound
{"execute":"guest-set-memory-blocks","arguments":{"mem-blks":[{"phys-index":0,"online":false}]}} @@ err:CommandNotFound
{"execute":"guest-suspend-disk"} @@ err:CommandNotFound
{"execute":"guest-suspend-ram"} @@ err:CommandNotFound
{"execute":"guest-suspend-hybrid"} @@ err:CommandNotFound
{"execute":"guest-set-time"} @@ err
{"execute":"guest-set-time","arguments":{"time":"not-a-number"}} @@ err:InvalidParameter
{"execute":"guest-exec","arguments":{}} @@ err:InvalidParameter
{"execute":"guest-exec","arguments":{"path":123}} @@ err:InvalidParameter
{"execute":"guest-exec","arguments":{"path":"/no/such/bin","capture-output":true}} @@ ok
{"execute":"guest-exec","arguments":{"path":"/usr/bin/true","capture-output":true}} @@ ok
{"execute":"guest-exec","arguments":{"path":"/usr/bin/wc","arg":["-c"],"input-data":"!!notbase64!!","capture-output":true}} @@ err:InvalidParameter
{"execute":"guest-exec-status","arguments":{}} @@ err:InvalidParameter
{"execute":"guest-exec-status","arguments":{"pid":999999}} @@ err:InvalidParameter
{"execute":"guest-file-open","arguments":{}} @@ err
{"execute":"guest-file-open","arguments":{"path":"/nonexistent/dir/x","mode":"r"}} @@ err
{"execute":"guest-file-read","arguments":{"handle":999999,"count":10}} @@ err:InvalidParameter
{"execute":"guest-file-write","arguments":{"handle":999999,"buf-b64":"QQ=="}} @@ err:InvalidParameter
{"execute":"guest-file-close","arguments":{"handle":999999}} @@ err:InvalidParameter
{"execute":"guest-file-seek","arguments":{"handle":999999,"offset":0,"whence":0}} @@ err:InvalidParameter
{"execute":"guest-file-flush","arguments":{"handle":999999}} @@ err:InvalidParameter
{"execute":"guest-ssh-get-authorized-keys","arguments":{}} @@ err
{"execute":"guest-ssh-get-authorized-keys","arguments":{"username":"nonexistent-user-xyz"}} @@ err
{"execute":"guest-ssh-add-authorized-keys","arguments":{}} @@ err
{"execute":"guest-ssh-remove-authorized-keys","arguments":{}} @@ err
{"execute":"guest-set-user-password","arguments":{}} @@ err
{"execute":"guest-set-user-password","arguments":{"username":"user","password":"!!notb64!!","crypted":false}} @@ err
{"execute":"no-such-command-at-all"} @@ err:CommandNotFound
{"badly":"formed-but-valid-json-no-execute"} @@ err
BATTERY_END

# QGA call with retry — the Tiger ISA-serial channel occasionally drops a single
# response under back-to-back load; retry up to 4x on an empty/non-JSON reply.
q() {
  local out
  for _ in 1 2 3 4; do
    out=$( (echo "$1"; sleep 2) | timeout 15 socat - UNIX-CONNECT:"$SOCK" 2>/dev/null )
    case "$out" in
      *'"return"'*|*'"error"'*) echo "$out"; return 0 ;;
    esac
    sleep 2
  done
  echo "$out"  # give caller whatever we last got (may be empty)
  return 1
}

# Strip assertions -> request stream; upload to guest in 1KB chunks (Tiger limit).
REQS="/tmp/mga-reqs-${VMID}.txt"
sed 's/ @@ .*//' "$BATT" > "$REQS"
EXPECT=$(wc -c < "$REQS" | tr -d ' ')
echo "[*] battery: $(wc -l < "$BATT") cases, ${EXPECT} bytes"
split -b 1024 "$REQS" "/tmp/mga-bc-${VMID}."

# guest-file-write WITHOUT q()'s retry (a retried write would duplicate bytes
# if only the response was lost). Instead, verify total size after upload and
# redo the whole truncating upload if it doesn't match.
upload_once() {
  local r h
  r=$( (echo "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"/private/var/tmp/.mga-battery\",\"mode\":\"w\"}}"; sleep 2) | timeout 15 socat - UNIX-CONNECT:"$SOCK" 2>/dev/null )
  h=$(echo "$r" | sed -n 's/.*"return":\([0-9]*\).*/\1/p')
  [ -z "$h" ] && return 1
  for f in "/tmp/mga-bc-${VMID}."*; do
    local cb; cb=$(base64 -w0 "$f" 2>/dev/null || base64 "$f" | tr -d '\n')
    (echo "{\"execute\":\"guest-file-write\",\"arguments\":{\"handle\":$h,\"buf-b64\":\"$cb\"}}"; sleep 2) | timeout 15 socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1
  done
  (echo "{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":$h}}"; sleep 2) | timeout 15 socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1
  return 0
}
uploaded=0
for attempt in 1 2 3 4; do
  upload_once
  # verify size in-guest
  vr=$(q "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"wc -c < /private/var/tmp/.mga-battery\"],\"capture-output\":true}}")
  vpid=$(echo "$vr" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')
  sleep 2
  got=$(q "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":$vpid}}" | python3 -c 'import json,sys,base64
try: print(base64.b64decode(json.load(sys.stdin)["return"].get("out-data","")).decode().strip())
except: print("")' 2>/dev/null)
  echo "    upload attempt $attempt: guest battery = ${got:-?} bytes (want $EXPECT)"
  [ "$got" = "$EXPECT" ] && { uploaded=1; break; }
done
rm -f "/tmp/mga-bc-${VMID}."*
[ "$uploaded" = 1 ] || { echo "FATAL: battery upload did not verify after 4 attempts"; exit 2; }

echo "[*] running '$AGENT --test' on the battery (run -> guest file -> cat)..."
# Run the battery to a guest file, then cat it. Poll until the exec exits
# (popen-based commands like diskstats/route make this take ~10-20s).
RUN=$(q "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"$AGENT --test < /private/var/tmp/.mga-battery > /private/var/tmp/.mga-out 2>/dev/null; cat /private/var/tmp/.mga-out\"],\"capture-output\":true}}")
PID=$(echo "$RUN" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')
[ -z "$PID" ] && { echo "FATAL: exec failed: $RUN"; exit 2; }
STAT=""
for _ in $(seq 1 25); do
  sleep 3
  STAT=$(q "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":$PID}}")
  echo "$STAT" | grep -q '"exited":true' && break
done
echo "$STAT" \
  | python3 -c 'import json,sys,base64,re
d=json.load(sys.stdin).get("return",{})
out=base64.b64decode(d.get("out-data","")).decode(errors="replace")
print("\n".join(re.sub(r"^QMP> ","",l) for l in out.splitlines() if l.strip()))' \
  > "/tmp/mga-out-${VMID}.txt"

echo "[*] asserting (jq) ..."
python3 - "$VMID" "$BATT" "/tmp/mga-out-${VMID}.txt" <<'PYEOF'
import json,sys,subprocess
vmid,battf,outf=sys.argv[1],sys.argv[2],sys.argv[3]
asserts=[]
for line in open(battf):
    line=line.rstrip("\n")
    if "@@" not in line: continue
    req,a=line.split("@@",1); asserts.append((req.strip(),a.strip()))
resp=[l for l in open(outf).read().splitlines() if l.strip()]
P=F=0; fails=[]
for i,(req,a) in enumerate(asserts):
    if i>=len(resp): F+=1; fails.append((req,a,"NO RESPONSE LINE")); continue
    # Strip any bytes before the first '{' — guest-sync-delimited intentionally
    # prefixes its reply with a 0xFF resync delimiter, which is not JSON.
    raw=resp[i]; brace=raw.find("{")
    raw=raw[brace:] if brace>=0 else raw
    try: r=json.loads(raw)
    except Exception: F+=1; fails.append((req,a,f"unparseable: {resp[i][:60]}")); continue
    ok=False; why=""
    if a=="ok": ok="return" in r; why="no .return: "+json.dumps(r)[:70]
    elif a=="err": ok="error" in r; why="no .error: "+json.dumps(r)[:70]
    elif a.startswith("err:"):
        cls=a[4:]; ok="error" in r and r["error"].get("class")==cls
        why=f"want err {cls}, got "+json.dumps(r)[:70]
    elif a.startswith("jq:"):
        ret=r.get("return")
        pr=subprocess.run(["jq","-e",a[3:]],input=json.dumps(ret),capture_output=True,text=True)
        ok=pr.returncode==0; why=f"jq false [{a[3:]}] on "+json.dumps(ret)[:70]
    else: why="unknown assertion form"
    if ok: P+=1
    else: F+=1; fails.append((req[:70],a,why))
print(f"\n=== VM {vmid}: {P} passed, {F} failed (of {len(asserts)} command cases) ===")
for req,a,why in fails: print(f"  FAIL {req}\n       [{a}] {why}")
sys.exit(1 if F else 0)
PYEOF
