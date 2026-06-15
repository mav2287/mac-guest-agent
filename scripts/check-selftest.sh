#!/bin/bash
# Ship gate: a binary must PASS its own --self-test-json AND --safe-test-json
# before it can be built/released. A binary that reports any failing self-test
# must never ship.
#
# This is the guard for the class of bug in issue #12: v2.5.5 shipped while its
# own `--safe-test-json` reported `20 passed, 1 failed, status:fail` (a stale
# self-test expectation for guest-fstrim). Nothing in the pipeline ran the
# binary's own self-test as a blocker, so it escaped. This script makes "ships
# while red" impossible — run it in build CI, release CI, and `make test`.
#
# Usage: scripts/check-selftest.sh [binary_path]   (default: ./build/mac-guest-agent)
set -u

BIN="${1:-./build/mac-guest-agent}"
if [ ! -x "$BIN" ]; then
    echo "check-selftest: binary not found or not executable: $BIN" >&2
    exit 2
fi

rc=0
for mode in self-test safe-test; do
    out="$("$BIN" --"${mode}"-json 2>/dev/null)"
    # status must be "pass"; failures/errors must be 0 (keys differ per mode:
    # self-test reports `errors`, safe-test reports `failures` — default to 0
    # for whichever key is absent so both shapes are checked the same way).
    if printf '%s' "$out" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception as e:
    print('  ${mode}: UNPARSEABLE JSON:', e); sys.exit(1)
status   = d.get('status')
failures = d.get('failures', 0)
errors   = d.get('errors', 0)
passes   = d.get('passes', '?')
print(f'  ${mode}: status={status} passes={passes} failures={failures} errors={errors}')
ok = (status == 'pass') and not failures and not errors
sys.exit(0 if ok else 1)
"; then
        :
    else
        echo "  ^^ ${mode} did not pass" >&2
        rc=1
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "SHIP GATE: PASS ($BIN passes its own self-test and safe-test)"
else
    echo "SHIP GATE: FAIL — $BIN reports failing self-tests; refusing to ship." >&2
fi
exit "$rc"
