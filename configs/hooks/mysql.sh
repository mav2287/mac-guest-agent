#!/bin/bash
# Freeze hook for MySQL / MariaDB
# Install: cp mysql.sh /etc/qemu/fsfreeze-hook.d/ && chmod 755 /etc/qemu/fsfreeze-hook.d/mysql.sh
#
# Requires: mysql client accessible as root, or configure credentials below.
# On freeze: flushes all tables and acquires a global read lock.
# On thaw: releases the lock.
#
# The read lock ensures no writes occur between freeze and snapshot.
# The lock is automatically released when the connection closes (thaw or timeout).

MYSQL="mysql"
MYSQL_OPTS="-u root"
LOCKFILE="/tmp/.fsfreeze-mysql-lock"

case "$1" in
    freeze)
        # Flush tables and acquire global read lock
        # Run in background so the lock persists until thaw
        $MYSQL $MYSQL_OPTS -e "FLUSH TABLES WITH READ LOCK; SYSTEM touch $LOCKFILE; SYSTEM sleep 600" &
        MYSQL_PID=$!
        echo "$MYSQL_PID" > /tmp/.fsfreeze-mysql-pid

        # Wait for the lock to be acquired (lockfile created)
        for i in $(seq 1 30); do
            [ -f "$LOCKFILE" ] && break
            sleep 1
        done

        if [ ! -f "$LOCKFILE" ]; then
            echo "mysql: failed to acquire read lock" >&2
            kill $MYSQL_PID 2>/dev/null
            exit 1
        fi
        ;;

    thaw)
        # Kill the background mysql session to release the lock
        if [ -f /tmp/.fsfreeze-mysql-pid ]; then
            kill $(cat /tmp/.fsfreeze-mysql-pid) 2>/dev/null
            rm -f /tmp/.fsfreeze-mysql-pid
        fi
        rm -f "$LOCKFILE"
        ;;
esac

exit 0
