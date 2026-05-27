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

# Check manpage template command count (LOW-4 in audit wave 5).
# The template line we're checking is "The agent registers N QGA commands".
# Counts get re-stamped into docs/mac-guest-agent.8 via make, so the
# template is the source of truth.
MANPAGE_TEMPLATE="docs/mac-guest-agent.8.in"
if [ -f "$MANPAGE_TEMPLATE" ]; then
    MANPAGE_CLAIM=$(grep -oE 'registers [0-9]+ QGA commands' "$MANPAGE_TEMPLATE" | grep -oE '[0-9]+' | head -1)
    if [ -n "$MANPAGE_CLAIM" ] && [ "$MANPAGE_CLAIM" != "$BINARY_COUNT" ]; then
        echo "ERROR: $MANPAGE_TEMPLATE claims $MANPAGE_CLAIM commands but binary has $BINARY_COUNT"
        echo "       Fix the template and run 'make docs/mac-guest-agent.8' to regenerate."
        ERRORS=1
    fi
fi

# Check ARCHITECTURE.md command count
ARCH_DOC="docs/ARCHITECTURE.md"
if [ -f "$ARCH_DOC" ]; then
    ARCH_CLAIM=$(grep -oE 'registry \([0-9]+ commands\)' "$ARCH_DOC" | grep -oE '[0-9]+' | head -1)
    if [ -n "$ARCH_CLAIM" ] && [ "$ARCH_CLAIM" != "$BINARY_COUNT" ]; then
        echo "ERROR: $ARCH_DOC claims $ARCH_CLAIM commands but binary has $BINARY_COUNT"
        ERRORS=1
    fi
fi

# Check that the freeze-list claim in COMMAND_STATUS.md hasn't reverted
# to the pre-implementation wording. The actual handler
# (src/cmd-fs.c handle_fsfreeze_freeze_list) filters by mountpoints;
# the prior doc said "mountpoint list not yet filtered" which is wrong.
if grep -q 'guest-fsfreeze-freeze-list.*mountpoint list not yet filtered' "$DOC_FILE"; then
    echo "ERROR: $DOC_FILE still claims guest-fsfreeze-freeze-list doesn't filter mountpoints,"
    echo "       but src/cmd-fs.c handle_fsfreeze_freeze_list() does filter. Update the row."
    ERRORS=1
fi

exit $ERRORS
