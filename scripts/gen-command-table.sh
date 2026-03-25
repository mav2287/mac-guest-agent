#!/bin/bash
# Generate command status table from the actual binary's guest-info output
# and cross-reference against docs/COMMAND_STATUS.md
#
# Usage: ./scripts/gen-command-table.sh [binary_path]
#
# Validates that the documented command list matches what the binary reports.
# Exits 1 if there's a mismatch.

set -e

BINARY="${1:-./build/mac-guest-agent}"

if [ ! -x "$BINARY" ]; then
    echo "Binary not found: $BINARY"
    echo "Run 'make build' first."
    exit 1
fi

echo "Checking commands from: $BINARY"
echo ""

# Get commands from the binary
BINARY_CMDS=$(echo '{"execute":"guest-info"}' | "$BINARY" --test 2>/dev/null \
    | sed 's/^QMP> //' \
    | python3 -c "
import json, sys
info = json.load(sys.stdin)['return']
print(info['version'])
for cmd in sorted(info['supported_commands'], key=lambda c: c['name']):
    status = 'enabled' if cmd['enabled'] else 'disabled'
    print(f\"{cmd['name']} {status}\")
")

BINARY_VERSION=$(echo "$BINARY_CMDS" | head -1)
BINARY_CMD_LIST=$(echo "$BINARY_CMDS" | tail -n +2)
BINARY_COUNT=$(echo "$BINARY_CMD_LIST" | wc -l | tr -d ' ')

echo "Agent version: $BINARY_VERSION"
echo "Commands registered: $BINARY_COUNT"
echo ""

# Get commands from COMMAND_STATUS.md
DOC_FILE="docs/COMMAND_STATUS.md"
if [ ! -f "$DOC_FILE" ]; then
    echo "WARNING: $DOC_FILE not found, skipping doc validation"
    exit 0
fi

DOC_CMDS=$(grep '| `guest-' "$DOC_FILE" | grep -v "Command\|example\|---" | sed 's/.*| `\(guest-[^`]*\)`.*/\1/' | sort)
DOC_COUNT=$(echo "$DOC_CMDS" | wc -l | tr -d ' ')

echo "Commands in $DOC_FILE: $DOC_COUNT"
echo ""

# Compare
BINARY_NAMES=$(echo "$BINARY_CMD_LIST" | awk '{print $1}' | sort)

MISSING_FROM_DOCS=$(comm -23 <(echo "$BINARY_NAMES") <(echo "$DOC_CMDS"))
MISSING_FROM_BINARY=$(comm -13 <(echo "$BINARY_NAMES") <(echo "$DOC_CMDS"))

ERRORS=0

if [ -n "$MISSING_FROM_DOCS" ]; then
    echo "ERROR: Commands in binary but NOT in docs:"
    echo "$MISSING_FROM_DOCS" | sed 's/^/  /'
    ERRORS=1
fi

if [ -n "$MISSING_FROM_BINARY" ]; then
    echo "ERROR: Commands in docs but NOT in binary:"
    echo "$MISSING_FROM_BINARY" | sed 's/^/  /'
    ERRORS=1
fi

if [ "$ERRORS" -eq 0 ]; then
    echo "OK: Binary and docs match ($BINARY_COUNT commands)"
fi

# Check count claims in docs
DOC_TOTAL_CLAIM=$(grep "^All [0-9]* registered" "$DOC_FILE" | grep -oE '[0-9]+' | head -1)
if [ -n "$DOC_TOTAL_CLAIM" ] && [ "$DOC_TOTAL_CLAIM" != "$BINARY_COUNT" ]; then
    echo "ERROR: $DOC_FILE claims $DOC_TOTAL_CLAIM commands but binary has $BINARY_COUNT"
    ERRORS=1
fi

# Check README command count
README_CLAIM=$(grep "[0-9]* registered QGA commands" README.md | grep -oE '[0-9]+' | head -1)
if [ -n "$README_CLAIM" ] && [ "$README_CLAIM" != "$BINARY_COUNT" ]; then
    echo "ERROR: README claims $README_CLAIM commands but binary has $BINARY_COUNT"
    ERRORS=1
fi

exit $ERRORS
