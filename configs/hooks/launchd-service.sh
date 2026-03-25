#!/bin/bash
# Freeze hook for generic launchd services
# Install: cp launchd-service.sh /etc/qemu/fsfreeze-hook.d/ && chmod 755 /etc/qemu/fsfreeze-hook.d/launchd-service.sh
#
# Configure SERVICES below with the launchd labels of services to stop during freeze.
# Services are stopped on freeze and restarted on thaw.
#
# Use this for any application that writes continuously and doesn't have
# its own freeze/thaw mechanism.

# Add service labels here (space-separated)
SERVICES="com.example.myapp com.example.worker"

case "$1" in
    freeze)
        for svc in $SERVICES; do
            launchctl stop "$svc" 2>/dev/null && echo "stopped: $svc"
        done
        # Brief pause to let services finish writing
        sleep 1
        ;;

    thaw)
        # Restart in reverse order
        for svc in $(echo $SERVICES | tr ' ' '\n' | tail -r); do
            launchctl start "$svc" 2>/dev/null && echo "started: $svc"
        done
        ;;
esac

exit 0
