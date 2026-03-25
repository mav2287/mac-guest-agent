#!/bin/bash
# Generate a COMPATIBILITY.md matrix row from verification JSON output
#
# Usage:
#   ./scripts/verify-installer.sh --json /path/to/installer.app | ./scripts/gen-matrix-row.sh
#   ./build/mac-guest-agent --self-test-json 2>/dev/null | ./scripts/gen-matrix-row.sh
#
# Reads JSON from stdin and outputs a markdown table row suitable for
# pasting into docs/COMPATIBILITY.md

set -e

INPUT=$(cat)

# Detect whether this is installer verification or self-test JSON
if echo "$INPUT" | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if 'checks' in d and 'version' in d else 1)" 2>/dev/null; then
    # Installer verification JSON
    python3 -c "
import json, sys

d = json.loads('''$INPUT''')
ver = d.get('version', '?')
codename = d.get('codename', '')
status = d.get('status', '?')
passes = d.get('passes', 0)
warns = d.get('warnings', 0)
fails = d.get('failures', 0)
total = passes + warns + fails

# Determine tier
tier = '2' if status == 'pass' else '3'

# Find kext info from checks
kext_ver = ''
for c in d.get('checks', []):
    if 'Apple16X50Serial.kext' in c.get('name', '') and c['level'] == 'pass':
        kext_ver = 'Kext present'
    if 'PCI class match' in c.get('name', '') and c['level'] == 'pass':
        kext_ver = 'Kext + PCI 0x0700'

# Determine binary type
arch = ''
for c in d.get('checks', []):
    if 'architectures' in c.get('name', '').lower():
        arch = c['name'].replace('Supported architectures: ', '').strip()
    if 'Agent binary' in c.get('name', ''):
        arch = c['name'].replace('Agent binary: ', '').strip()

evidence = f'Deep verify {passes}/{total} ({__import__(\"datetime\").date.today()})'

name = f'{ver} {codename}'.strip()
print(f'| **{name}** | **{tier}** | **{arch}** | Untested | Untested | Untested | **{kext_ver}** | Untested | **{evidence}** |')
"
elif echo "$INPUT" | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if 'system_info' in d else 1)" 2>/dev/null; then
    # Self-test JSON
    python3 -c "
import json, sys, datetime

d = json.loads('''$INPUT''')
si = d['system_info']
ver = si.get('os_version', '?')
arch = si.get('arch', '?')
kext = si.get('serial_kext_version', '?')
freeze = si.get('freeze_method', '?')
cmds = si.get('command_count', '?')
errors = d.get('errors', 0)
warns = d.get('warnings', 0)
passes = d.get('passes', 0)

status = 'Yes' if errors == 0 else 'No'
today = datetime.date.today()

print(f'| **{ver}** | **1** | **{arch}** | **{status}** | **{status}** | **Pending** | **Kext v{kext}** | **{freeze}** | **Self-test {passes}p/{warns}w/{errors}e, {cmds} commands ({today})** |')
"
else
    echo "ERROR: Unrecognized JSON format" >&2
    echo "Expected output from --self-test-json or verify-installer.sh --json" >&2
    exit 1
fi
