#!/bin/bash
# Local --test battery runner: feeds the exhaustive request set straight into
# `mac-guest-agent --test` (no QGA socket) and asserts each response with jq,
# reusing the exact "REQUEST @@ ASSERTION" grammar from exhaustive-commands.sh.
# For SSH-only targets (e.g. the arm64 tart VM) where there is no QGA channel.
set -u
export LC_ALL=C   # guest-sync-delimited emits a leading 0xFF byte; keep sed/grep byte-safe
AGENT="${1:-/usr/local/bin/mac-guest-agent}"
BATT="$(mktemp)"
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

REQS="$(mktemp)"; sed 's/ @@ .*//' "$BATT" > "$REQS"
OUT="$(mktemp)"
# strip any leading prefix (QMP> banner and/or 0xFF delimiter) before the JSON,
# uniformly, so the response stream stays 1:1 with the request stream.
"$AGENT" --test < "$REQS" 2>/dev/null | sed 's/^[^{[]*//' | grep '^[{[]' > "$OUT" || true

PASS=0; FAIL=0; i=0
while IFS= read -r line; do
  i=$((i+1))
  req="${line%% @@ *}"; assert="${line##* @@ }"
  resp="$(sed -n "${i}p" "$OUT")"
  cmd="$(printf '%s' "$req" | sed -n 's/.*"execute":"\([^"]*\)".*/\1/p')"; [ -z "$cmd" ] && cmd="(no-exec)"
  verdict=FAIL; why=""
  case "$assert" in
    ok)
      printf '%s' "$resp" | jq -e 'has("return")' >/dev/null 2>&1 && verdict=PASS || why="expected return, got: $resp" ;;
    err:*)
      class="${assert#err:}"
      got="$(printf '%s' "$resp" | jq -r '.error.class // empty' 2>/dev/null)"
      [ "$got" = "$class" ] && verdict=PASS || why="expected err class $class, got: $resp" ;;
    err)
      printf '%s' "$resp" | jq -e 'has("error")' >/dev/null 2>&1 && verdict=PASS || why="expected error, got: $resp" ;;
    jq:*)
      expr="${assert#jq:}"
      if printf '%s' "$resp" | jq -e "(.return) as \$r | \$r | ($expr)" >/dev/null 2>&1; then
        verdict=PASS
      else
        why="jq '$expr' false/err on: $resp"
      fi ;;
  esac
  if [ "$verdict" = PASS ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "  FAIL [$i] $cmd :: $why"; fi
done < "$BATT"

echo "LOCAL BATTERY: $PASS passed, $FAIL failed (of $i)"
rm -f "$BATT" "$REQS" "$OUT"
[ "$FAIL" = 0 ]
