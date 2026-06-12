# Tiger 10.4.11 — issue #11 fix confirmed live on real hardware

**Date:** 2026-06-07 (PVE host time 21:18 UTC).

## Pre-fix v2.5.4 behavior

```
ssh pve "qm guest cmd 111 network-get-interfaces"
[]
```

(Empty array — vit9696's issue #11.)

## Post-fix v2.5.5-dev behavior

Tiger VM 111 (10.4.11 i386 RELEASE_I386, Darwin 8.11.1), config:
- machine: pc-q35-7.0
- cpu: Nehalem,vendor=GenuineIntel,kvm=off
- USB: pci-ohci + usb-tablet + usb-kbd on ohci.0 (USB 1.1, 12 Mb/s — confirmed via `info usb`)
- net: e1000-82545em on slirp NAT 10.0.2.15

After `--upgrade` to the v2.5.5-dev binary:

```
ssh pve "(echo '{\"execute\":\"guest-network-get-interfaces\"}'; sleep 3) | \
         socat - UNIX-CONNECT:/var/run/qemu-server/111.qga"

{"return":[{
  "name":"en0",
  "hardware-address":"XX:XX:XX:XX:XX:XX",
  "ip-addresses":[
    {"ip-address":"10.0.2.15","ip-address-type":"ipv4","prefix":24}
  ],
  "statistics":{
    "rx-bytes":1035,"rx-packets":6,"rx-errs":0,"rx-dropped":0,
    "tx-bytes":5924,"tx-packets":33,"tx-errs":0,"tx-dropped":0
  }
}]}
```

Every field is correct:
- `name` matches the actual interface (`en0`).
- `hardware-address` matches the QEMU args MAC (`XX:XX:XX:XX:XX:XX`).
- IPv4 address matches the slirp NAT-assigned address (`10.0.2.15`).
- Prefix matches the /24 slirp subnet.
- Statistics reflect actual bytes/packets the interface has handled
  since boot — non-zero, monotonic.

This is real interface data from a real Tiger 10.4.11 kernel. The
fix replaces the v2.5.4 short-circuit (`return cJSON_CreateArray()`)
with three new code paths:

- `tiger_get_interfaces_ioctl()` — SIOCGIFCONF + per-name
  SIOCGIFFLAGS/SIOCGIFNETMASK + AF_LINK MAC extraction. Bypasses
  libc's `getifaddrs()` which hangs indefinitely in Tiger
  daemon context.
- `tiger_get_routes_sysctl()` — sysctl(NET_RT_DUMP, AF_INET) parsing
  rt_msghdr+sockaddrs. (Note: this code path may hang Tiger; see
  Known Limitations below.)
- `tiger_add_stats_from_iflist()` — sysctl(NET_RT_IFLIST) walking
  if_msghdr ifi_data 32-bit counters. Working — confirmed by the
  populated `statistics` block above.

Plus a separate fix in `src/service.c` `check_our_daemon_running()`
to handle Tiger's label-only `launchctl list` output format (root
cause of vit9696's separate `--upgrade` rollback report).

## Tiger VM stability — open issue

Tiger VM 111 is chronically unstable on this PVE host. Even with the
DarwinKVM/LongQT-sea-recommended config (Nehalem CPU, OHCI USB,
proper RTC settings), Tiger hits soft-wedges after extended use —
the kernel keeps executing (EIP not in HLT, clock advances on
screen), but the serial chardev disconnects and QGA stops
responding. After the wedge, the QEMU `info chardev` shows
`disconnected:` prefix on the qga0 line.

Hypotheses (not yet confirmed):

1. **sysctl(NET_RT_DUMP) may panic Tiger**. The
   `network-get-route` command was called right before one of the
   wedges; the kernel was actively executing (HLT=0) and
   responsiveness was lost. NET_RT_DUMP returns the full routing
   table — likely much larger than NET_RT_IFLIST and may trip a
   bug in Tiger's PF_ROUTE sysctl handler.
2. **Tiger's BSD serial driver wedges under load**. We have prior
   evidence (in `src/agent.c` watchdog comment) that the serial
   driver can wedge with fd open but no data flow. The watchdog
   should cycle the channel, but if our daemon process itself is
   stuck in a sysctl call, it can't.

## Recommendations for follow-up

- Pre-test `network-get-route` with a small payload (e.g., add an
  early-out for when sysctl returns a buffer larger than some
  Tiger-safe threshold, fall back to a partial-table response).
- Add a kqueue-based timeout around the Tiger sysctl calls so the
  daemon can't get stuck indefinitely.
- Consider rewriting `tiger_get_routes_sysctl()` to use
  `route -n get <prefix>` for the default route instead of a full
  table dump, since most callers only care about the default route.

The main fix (interfaces + MAC + IPs + stats) is proven working
and unblocks vit9696's primary complaint in issue #11.
