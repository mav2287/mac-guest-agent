#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>

static char *format_mac(const struct sockaddr_dl *sdl)
{
    if (sdl->sdl_alen != 6) return NULL;
    const unsigned char *mac = (const unsigned char *)LLADDR(sdl);
    char *buf = malloc(18);
    if (buf) {
        snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return buf;
}

/* True iff s is non-empty and entirely ASCII digits. */
static int is_all_digits(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Parse netstat -ibn output for one interface name and attach a "statistics"
 * object. `netstat_out` is captured ONCE by the caller and passed per
 * interface (no per-interface fork). Used on every macOS version.
 *
 * netstat -ibn columns:
 *   Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll
 * The Address column is BLANK on the <Link#N> summary line of interfaces with
 * no link-layer address (lo0, gif/stf tunnels). A positional sscanf that
 * assumes Address is always present then shifts every counter one column to
 * the left — e.g. lo0 reported Ibytes as rx-errs and Ipkts as rx-bytes. So we
 * tokenize and anchor on the "<Link" Network token: the counters begin at the
 * next token, after skipping a link-layer address if one is present (a MAC is
 * not all-digits; the first counter Ipkts is). */
static void add_net_stats(cJSON *iface_obj, const char *name,
                          const char *netstat_out)
{
    if (!netstat_out) return;

    /* strtok_r mutates its input; work on a copy so we don't trash the
     * caller's buffer between per-interface calls. */
    char *copy = strdup(netstat_out);
    if (!copy) return;

    char *save_ptr = NULL;
    char *line = strtok_r(copy, "\n", &save_ptr);
    while (line) {
        char line_name[64];
        /* Match this interface's <Link#N> aggregate line (carries the
         * counters); the per-address lines show Ierrs/Oerrs as "-". */
        if (sscanf(line, "%63s", line_name) == 1 && strcmp(line_name, name) == 0
                && strstr(line, "<Link")) {
            char *lcopy = strdup(line);
            if (!lcopy) break;
            char *tsave = NULL;
            char *toks[40];
            int nt = 0;
            for (char *tk = strtok_r(lcopy, " \t", &tsave); tk && nt < 40;
                 tk = strtok_r(NULL, " \t", &tsave))
                toks[nt++] = tk;

            int li = -1;
            for (int k = 0; k < nt; k++)
                if (strncmp(toks[k], "<Link", 5) == 0) { li = k; break; }

            if (li >= 0) {
                int j = li + 1;
                /* skip the Address column iff present (MAC: not all-digits) */
                if (j < nt && !is_all_digits(toks[j])) j++;
                if (j + 6 <= nt
                        && is_all_digits(toks[j])   && is_all_digits(toks[j+1])
                        && is_all_digits(toks[j+2]) && is_all_digits(toks[j+3])
                        && is_all_digits(toks[j+4]) && is_all_digits(toks[j+5])) {
                    long long ipkts  = strtoll(toks[j],   NULL, 10);
                    long long ierrs  = strtoll(toks[j+1], NULL, 10);
                    long long ibytes = strtoll(toks[j+2], NULL, 10);
                    long long opkts  = strtoll(toks[j+3], NULL, 10);
                    long long oerrs  = strtoll(toks[j+4], NULL, 10);
                    long long obytes = strtoll(toks[j+5], NULL, 10);
                    cJSON *stats = cJSON_CreateObject();
                    cJSON_AddNumberToObject(stats, "rx-bytes", (double)ibytes);
                    cJSON_AddNumberToObject(stats, "rx-packets", (double)ipkts);
                    cJSON_AddNumberToObject(stats, "rx-errs", (double)ierrs);
                    cJSON_AddNumberToObject(stats, "rx-dropped", 0);
                    cJSON_AddNumberToObject(stats, "tx-bytes", (double)obytes);
                    cJSON_AddNumberToObject(stats, "tx-packets", (double)opkts);
                    cJSON_AddNumberToObject(stats, "tx-errs", (double)oerrs);
                    cJSON_AddNumberToObject(stats, "tx-dropped", 0);
                    cJSON_AddItemToObject(iface_obj, "statistics", stats);
                    free(lcopy);
                    free(copy);
                    return;
                }
            }
            free(lcopy);
        }
        line = strtok_r(NULL, "\n", &save_ptr);
    }
    free(copy);
}

static cJSON *handle_network_get_interfaces(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    /* Native getifaddrs(3) on all versions. (A Tiger 10.4 special-case that
     * bypassed getifaddrs with a direct SIOCGIFCONF ioctl walk was removed in
     * v2.5.5: it existed because getifaddrs hung when the daemon ran the x86_64
     * slice, but with the daemon always installed as i386 on Tiger getifaddrs
     * returns instantly — confirmed under a launchd daemon on both the real
     * i386 iMac and the QEMU i386 VM. See docs/evidence/v2.5.5.) */
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get network interfaces";
        return NULL;
    }

    /* Capture netstat -ibn output ONCE for the whole response. Each
     * per-interface stats lookup parses the cached buffer instead of
     * forking netstat N times. (Tiger uses this same getifaddrs path as of
     * v2.5.5 — the old SIOCGIFCONF / NET_RT_IFLIST special-case was removed.) */
    char *netstat_out = NULL;
    (void)run_command_capture("netstat -ibn", &netstat_out);

    /* Build interface map: name -> {mac, ips} */
    cJSON *result = cJSON_CreateArray();

    /* First pass: collect unique interface names that are up (loopback
     * included, to match Linux qemu-ga which reports lo). Cap is generous —
     * modern macOS can have many utun/bridge/awdl interfaces; log if we hit it
     * so a silent truncation can't masquerade as the full set. */
#define MAX_IFACES 64
    char seen_names[MAX_IFACES][64];
    int seen_count = 0;

    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        int found = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen_names[i], ifa->ifa_name) == 0) { found = 1; break; }
        }
        if (!found) {
            if (seen_count >= MAX_IFACES) {
                LOG_DEBUG("network-get-interfaces: more than %d interfaces; list truncated", MAX_IFACES);
                break;
            }
            strncpy(seen_names[seen_count], ifa->ifa_name, 63);
            seen_names[seen_count][63] = '\0';
            seen_count++;
        }
    }

    /* Build JSON for each interface */
    for (int i = 0; i < seen_count; i++) {
        cJSON *iface = cJSON_CreateObject();
        cJSON_AddStringToObject(iface, "name", seen_names[i]);

        cJSON *ip_arr = cJSON_CreateArray();

        for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || strcmp(ifa->ifa_name, seen_names[i]) != 0)
                continue;

            if (!ifa->ifa_addr) continue;

            /* MAC address via AF_LINK */
            if (ifa->ifa_addr->sa_family == AF_LINK) {
                struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
                if (sdl->sdl_alen == 6) {
                    char *mac = format_mac(sdl);
                    if (mac) {
                        cJSON_AddStringToObject(iface, "hardware-address", mac);
                        free(mac);
                    }
                }
            }

            /* IPv4 */
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                char addr_buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf));

                int prefix = 0;
                if (ifa->ifa_netmask) {
                    struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;
                    uint32_t m = ntohl(mask->sin_addr.s_addr);
                    while (m & 0x80000000) { prefix++; m <<= 1; }
                }

                cJSON *ip = cJSON_CreateObject();
                cJSON_AddStringToObject(ip, "ip-address", addr_buf);
                cJSON_AddStringToObject(ip, "ip-address-type", "ipv4");
                cJSON_AddNumberToObject(ip, "prefix", prefix);
                cJSON_AddItemToArray(ip_arr, ip);
            }

            /* IPv6 */
            if (ifa->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
                char addr_buf[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, sizeof(addr_buf));

                int prefix = 0;
                if (ifa->ifa_netmask) {
                    struct sockaddr_in6 *mask6 = (struct sockaddr_in6 *)ifa->ifa_netmask;
                    for (int b = 0; b < 16; b++) {
                        unsigned char byte = mask6->sin6_addr.s6_addr[b];
                        while (byte & 0x80) { prefix++; byte <<= 1; }
                        if (byte != 0) break;
                    }
                }

                cJSON *ip = cJSON_CreateObject();
                cJSON_AddStringToObject(ip, "ip-address", addr_buf);
                cJSON_AddStringToObject(ip, "ip-address-type", "ipv6");
                cJSON_AddNumberToObject(ip, "prefix", prefix);
                cJSON_AddItemToArray(ip_arr, ip);
            }
        }

        cJSON_AddItemToObject(iface, "ip-addresses", ip_arr);
        add_net_stats(iface, seen_names[i], netstat_out);
        cJSON_AddItemToArray(result, iface);
    }

    freeifaddrs(ifap);
    free(netstat_out);
    LOG_DEBUG("Retrieved %d network interfaces", cJSON_GetArraySize(result));
    return result;
}

/* Format an IPv4 prefix length as a dotted-quad netmask string.
 * prefix=24 → "255.255.255.0". Out of range → "0.0.0.0". */
static void ipv4_prefix_to_mask(int prefix, char *out, size_t outsz)
{
    if (prefix < 0 || prefix > 32) prefix = 0;
    uint32_t mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    snprintf(out, outsz, "%u.%u.%u.%u",
             (mask >> 24) & 0xFFu, (mask >> 16) & 0xFFu,
             (mask >> 8)  & 0xFFu,  mask        & 0xFFu);
}

/* Format an IPv6 prefix length as a colon-separated hex netmask string.
 * prefix=64 → "ffff:ffff:ffff:ffff:0000:0000:0000:0000". Out of range → all-zero. */
static void ipv6_prefix_to_mask(int prefix, char *out, size_t outsz)
{
    if (prefix < 0 || prefix > 128) prefix = 0;
    unsigned char bytes[16] = {0};
    int full = prefix / 8;
    int rem  = prefix % 8;
    for (int i = 0; i < full && i < 16; i++) bytes[i] = 0xFFu;
    if (rem > 0 && full < 16) bytes[full] = (unsigned char)(0xFFu << (8 - rem));
    snprintf(out, outsz,
             "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             bytes[0],  bytes[1],  bytes[2],  bytes[3],
             bytes[4],  bytes[5],  bytes[6],  bytes[7],
             bytes[8],  bytes[9],  bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}


/* Zero-extend an abbreviated IPv4 destination to a full dotted quad:
 * "10.37.129" -> "10.37.129.0", "127" -> "127.0.0.0". A full 4-octet address
 * is unchanged. macOS `netstat -rn` abbreviates network routes by dropping
 * trailing zero octets, in BOTH the slashless ("127") and slash ("10.37.129/24")
 * forms, so the destination must be expanded in both. */
static void ipv4_zero_extend(const char *partial, char *out, size_t outsz)
{
    int o0 = 0, o1 = 0, o2 = 0, o3 = 0;
    sscanf(partial, "%d.%d.%d.%d", &o0, &o1, &o2, &o3);
    snprintf(out, outsz, "%d.%d.%d.%d", o0, o1, o2, o3);
}

static cJSON *handle_network_get_route(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    /* Parse `netstat -rn` for the routing table on all versions. This was
     * historically chosen over sysctl(NET_RT_DUMP) because the full-table
     * NET_RT_DUMP hung the Tiger daemon — but that was an x86_64-on-Tiger
     * artifact: NET_RT_DUMP returns instantly under an i386 daemon (confirmed
     * v2.5.5 on the iMac and the QEMU VM, see docs/evidence/v2.5.5). We keep
     * the netstat-text path anyway: it's the single, version-independent
     * implementation and its output is straightforward to parse. */

    char *out = NULL;
    if (run_command_capture("netstat -rn", &out) != 0 || !out) {
        free(out);
        *err_class = "GenericError";
        *err_desc = "Failed to get routing table";
        return NULL;
    }

    cJSON *result = cJSON_CreateArray();
    int in_inet4 = 0, in_inet6 = 0;

    char *save_ptr = NULL;
    char *line = strtok_r(out, "\n", &save_ptr);
    while (line) {
        /* Detect section headers */
        if (strstr(line, "Internet:") && !strstr(line, "Internet6:")) {
            in_inet4 = 1; in_inet6 = 0;
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }
        if (strstr(line, "Internet6:")) {
            in_inet4 = 0; in_inet6 = 1;
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }

        /* Skip headers and empty lines */
        if (strstr(line, "Destination") || strstr(line, "Routing tables") || line[0] == '\0') {
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }

        if (!in_inet4 && !in_inet6) {
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }

        /* Parse route line.
         *
         * macOS netstat -rn output differs by version:
         *   10.5+ (4 columns):  Destination Gateway Flags        Netif [Expire]
         *   10.4   (6 columns): Destination Gateway Flags Refs Use Netif [Expire]
         *
         * The Tiger row has two extra accounting columns (Refs / Use)
         * after Flags that the modern row omits. Parsing the first 4
         * `%s` tokens gets the Netif on 10.5+ but grabs the Refs
         * counter on 10.4 — that's the v2.5.5 bug where Tiger reported
         * `"iface":"3"` instead of `"iface":"en0"`. Resolve by trying
         * the wider 6-column shape first and falling back to the
         * narrower 4-column shape if the 6th token isn't present. */
        char dest[128] = "", gateway[128] = "", flags[32] = "";
        char netif[32] = "", t4[32] = "", t5[32] = "";
        int n = sscanf(line, "%127s %127s %31s %31s %31s %31s",
                       dest, gateway, flags, t4, t5, netif);
        if (n >= 6) {
            /* Tiger 6-column form: t4=Refs, t5=Use, netif=Netif. */
        } else if (n >= 4) {
            /* Modern 4-column form: t4 was actually Netif. */
            strncpy(netif, t4, sizeof(netif) - 1);
            netif[sizeof(netif) - 1] = '\0';
        } else if (n < 3) {
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }

        /* Field shapes here track the QGA GuestNetworkRoute schema:
         *   iface, destination, mask, metric, gateway, irtt, version,
         *   desprefixlen, nexthop.
         * macOS's `netstat -rn` doesn't expose a routing-table `metric`
         * column at all (would require per-route `route -n get`) and has
         * no `irtt` concept; we emit 0 for both — same precedent as
         * cpustats' `nice: 0` on macOS — spec-conformant with honest
         * zeros for fields the host can't supply. */

        /* Derive the prefix length first — destination may carry /CIDR
         * suffix, may be "default", or may be an abbreviated form like
         * "127" / "192.168.1" that macOS netstat shows for legacy
         * classful and CIDR routes without an explicit suffix. */
        int prefix_int;
        const char *prefix_str;
        char prefix_buf[8] = "";
        char dest_clean[128];
        strncpy(dest_clean, dest, sizeof(dest_clean) - 1);
        dest_clean[sizeof(dest_clean) - 1] = '\0';
        char *slash = strchr(dest_clean, '/');
        if (slash) {
            *slash = '\0';
            prefix_int = atoi(slash + 1);
            /* Copy the prefix into its own buffer: prefix_str must NOT alias
             * dest_clean, which the zero-extend below overwrites. */
            snprintf(prefix_buf, sizeof(prefix_buf), "%d", prefix_int);
            prefix_str = prefix_buf;
            /* The network part is abbreviated too: "10.37.129/24" and
             * "224.0.0/4" carry a short destination that must be zero-extended
             * ("10.37.129.0", "224.0.0.0"). IPv6 destinations aren't octet-based. */
            if (in_inet4) {
                char ext[64];
                ipv4_zero_extend(dest_clean, ext, sizeof(ext));
                snprintf(dest_clean, sizeof(dest_clean), "%s", ext);
            }
        } else if (strcmp(dest_clean, "default") == 0) {
            /* "default" is the zero-prefix any-destination route. */
            snprintf(dest_clean, sizeof(dest_clean), "%s", in_inet4 ? "0.0.0.0" : "::");
            prefix_str = "0";
            prefix_int = 0;
        } else if (in_inet4) {
            /* IPv4 with no explicit /CIDR. macOS `netstat -rn` abbreviates a
             * network route by dropping trailing zero octets, so the octet
             * count implies the prefix: "127" = 127.0.0.0/8, "169.254" =
             * 169.254.0.0/16, "10.0.2" = 10.0.2.0/24, and a full dotted quad
             * is a /32 host route. Zero-extend the address and derive the
             * prefix from the octet count (matches BSD netname()). Without
             * this, "127" and "169.254" were reported as bogus /32 hosts. */
            int o0 = 0, o1 = 0, o2 = 0, o3 = 0;
            int oc = sscanf(dest_clean, "%d.%d.%d.%d", &o0, &o1, &o2, &o3);
            if (oc < 1) {
                /* Not a parseable IPv4 destination — skip the row rather than
                 * fabricate a bogus 0.0.0.0/8 route. */
                line = strtok_r(NULL, "\n", &save_ptr);
                continue;
            }
            if (oc > 4) oc = 4;
            prefix_int = oc * 8;
            snprintf(dest_clean, sizeof(dest_clean), "%d.%d.%d.%d", o0, o1, o2, o3);
            snprintf(prefix_buf, sizeof(prefix_buf), "%d", prefix_int);
            prefix_str = prefix_buf;
        } else {
            /* IPv6 with no /CIDR is a host route; netstat shows explicit /N
             * for IPv6 networks. */
            prefix_str = "128";
            prefix_int = 128;
        }

        char mask[64] = "";
        if (in_inet4) ipv4_prefix_to_mask(prefix_int, mask, sizeof(mask));
        else           ipv6_prefix_to_mask(prefix_int, mask, sizeof(mask));

        cJSON *route = cJSON_CreateObject();
        cJSON_AddStringToObject(route, "iface",        netif);
        cJSON_AddStringToObject(route, "destination",  dest_clean);
        cJSON_AddStringToObject(route, "gateway",      gateway);
        cJSON_AddStringToObject(route, "nexthop",      gateway);     /* spec allows both; on macOS they're the same */
        cJSON_AddStringToObject(route, "mask",         mask);
        cJSON_AddNumberToObject(route, "metric",       0);            /* macOS netstat doesn't expose route metric */
        cJSON_AddNumberToObject(route, "irtt",         0);            /* Linux-only concept; constant 0 on macOS */
        cJSON_AddNumberToObject(route, "version",      in_inet4 ? 4 : 6);
        cJSON_AddStringToObject(route, "desprefixlen", prefix_str);

        cJSON_AddItemToArray(result, route);

        line = strtok_r(NULL, "\n", &save_ptr);
    }

    free(out);
    LOG_DEBUG("Retrieved %d routes", cJSON_GetArraySize(result));
    return result;
}

void cmd_network_init(void)
{
    command_register("guest-network-get-interfaces", handle_network_get_interfaces, 1);
    command_register("guest-network-get-route", handle_network_get_route, 1);
}
