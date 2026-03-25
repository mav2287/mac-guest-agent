#!/bin/bash
# Freeze hook for Redis
# Install: cp redis.sh /etc/qemu/fsfreeze-hook.d/ && chmod 755 /etc/qemu/fsfreeze-hook.d/redis.sh
#
# On freeze: triggers a BGSAVE to persist the dataset to disk.
# On thaw: nothing (Redis resumes automatically).
#
# Redis is designed to recover from its RDB/AOF files, so this hook
# ensures the latest data is persisted before the snapshot.

REDIS_CLI="redis-cli"

case "$1" in
    freeze)
        # Trigger background save
        $REDIS_CLI BGSAVE 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "redis: BGSAVE failed (is Redis running?)" >&2
            exit 0
        fi

        # Wait for save to complete (up to 25 seconds)
        for i in $(seq 1 25); do
            status=$($REDIS_CLI LASTSAVE 2>/dev/null)
            if [ $? -eq 0 ]; then
                # Check if BGSAVE is still in progress
                bg=$($REDIS_CLI INFO persistence 2>/dev/null | grep rdb_bgsave_in_progress | tr -d '\r' | cut -d: -f2)
                [ "$bg" = "0" ] && break
            fi
            sleep 1
        done
        ;;

    thaw)
        # Nothing to do — Redis resumes automatically
        ;;
esac

exit 0
