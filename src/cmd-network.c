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

static void add_net_stats(cJSON *iface_obj, const char *name)
{
    char *out = NULL;
    if (run_command_capture("netstat -ibn", &out) != 0 || !out) {
        free(out);
        return;
    }

    char *save_ptr = NULL;
    char *line = strtok_r(out, "\n", &save_ptr);
    while (line) {
        /* Skip non-matching lines */
        char line_name[64];
        if (sscanf(line, "%63s", line_name) == 1 && strcmp(line_name, name) == 0) {
            /* netstat -ibn format varies, look for numeric fields */
            /* Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll */
            char n[64];
            int mtu;
            char net[64], addr[64];
            long long ipkts, ierrs, ibytes, opkts, oerrs, obytes;

            if (sscanf(line, "%63s %d %63s %63s %lld %lld %lld %lld %lld %lld",
                       n, &mtu, net, addr, &ipkts, &ierrs, &ibytes, &opkts, &oerrs, &obytes) >= 10) {
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
                free(out);
                return;
            }
        }
        line = strtok_r(NULL, "\n", &save_ptr);
    }
    free(out);
}

static cJSON *handle_network_get_interfaces(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get network interfaces";
        return NULL;
    }

    /* Build interface map: name -> {mac, ips} */
    cJSON *result = cJSON_CreateArray();

    /* First pass: collect unique interface names that are up and not loopback */
    char seen_names[32][64];
    int seen_count = 0;

    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        int found = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen_names[i], ifa->ifa_name) == 0) { found = 1; break; }
        }
        if (!found && seen_count < 32) {
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
        add_net_stats(iface, seen_names[i]);
        cJSON_AddItemToArray(result, iface);
    }

    freeifaddrs(ifap);
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

static cJSON *handle_network_get_route(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

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

        /* Parse route line: Destination Gateway Flags Netif [Expire] */
        char dest[128] = "", gateway[128] = "", flags[32] = "", netif[32] = "";
        if (sscanf(line, "%127s %127s %31s %31s", dest, gateway, flags, netif) < 3) {
            line = strtok_r(NULL, "\n", &save_ptr);
            continue;
        }

        /* Field shapes here track the QGA GuestNetworkRoute schema:
         *   iface, destination, mask, metric, gateway, irtt, version,
         *   desprefixlen, nexthop.
         * macOS's `netstat -rn` doesn't expose a routing-table `metric`
         * column at all (would require per-route `route -n get`) and has
         * no `irtt` concept; we emit 0 for both — same precedent as
         * cpustats' `nice: 0` on macOS (audit.md finding 2b, Q4-style
         * spec-conformant-with-honest-zeros). */

        /* Derive the prefix length first — destination may carry /CIDR
         * suffix, may be "default", or may be an abbreviated form like
         * "127" / "192.168.1" that macOS netstat shows for legacy
         * classful and CIDR routes without an explicit suffix. */
        int prefix_int;
        const char *prefix_str;
        char dest_clean[128];
        strncpy(dest_clean, dest, sizeof(dest_clean) - 1);
        dest_clean[sizeof(dest_clean) - 1] = '\0';
        char *slash = strchr(dest_clean, '/');
        if (slash) {
            *slash = '\0';
            prefix_str = slash + 1;
            prefix_int = atoi(prefix_str);
        } else if (strcmp(dest_clean, "default") == 0) {
            /* "default" is the zero-prefix any-destination route. */
            snprintf(dest_clean, sizeof(dest_clean), "%s", in_inet4 ? "0.0.0.0" : "::");
            prefix_str = "0";
            prefix_int = 0;
        } else {
            /* Host route — no CIDR suffix, full-length prefix. */
            prefix_str = in_inet4 ? "32" : "128";
            prefix_int = in_inet4 ? 32 : 128;
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
