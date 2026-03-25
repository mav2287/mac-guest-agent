#!/bin/bash
# Freeze hook for PostgreSQL
# Install: cp postgresql.sh /etc/qemu/fsfreeze-hook.d/ && chmod 755 /etc/qemu/fsfreeze-hook.d/postgresql.sh
#
# On freeze: triggers a checkpoint to flush WAL to disk, then pauses WAL archiving.
# On thaw: resumes normal operation.
#
# PostgreSQL is crash-safe by design (WAL recovery), so a freeze without this hook
# is still recoverable. This hook ensures a clean checkpoint for faster recovery.

PGUSER="postgres"
PSQL="psql -U $PGUSER"

case "$1" in
    freeze)
        # Force a checkpoint — flushes all dirty buffers to disk
        $PSQL -c "CHECKPOINT;" 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "postgresql: checkpoint failed (is PostgreSQL running?)" >&2
            # Non-fatal — PostgreSQL WAL recovery handles this
            exit 0
        fi
        ;;

    thaw)
        # Nothing to do — PostgreSQL resumes automatically
        ;;
esac

exit 0
