// route-test: the routes an app reads (ADR-0054 step c3): RTM_GETROUTE's dump for IPv4 and IPv6 on a NETLINK_ROUTE
// socket, as Android 15 lays the tables out (the kernel's local and main tables, and netd's table of each network,
// 1000 + its interface index, and of its local routes, 1000000000 + the index). Every interface but lo differs from
// device to device, so the fixture prints lo's routes whole and, for the others, only whether each route that
// Android's layout has for an address, a subnet or a default route is there, as it is: no address, gateway or index.
// An interface counts if getifaddrs has an IPv4 address or a global IPv6 address for it; the emulator's dummy0, which
// has only a link-local one, does not. On the emulator the fixture runs as the shell, whose dump is an app's. It
// waits up to 30 s for the default routes of each such interface, which netd adds after the boot.
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_ROUTES 512
#define MAX_ATTRIBUTES 16

struct route {
    struct rtmsg header;
    unsigned table;
    unsigned char destination[16], gateway[16], source[16];
    int has_destination, has_gateway, has_source, has_priority;
    unsigned oif, priority;
    unsigned short attributes[MAX_ATTRIBUTES];
    int attribute_count;
};

static struct route routes[MAX_ROUTES];
static int route_count;

static const char *attribute_name(unsigned short type) {
    switch (type) {
        case RTA_DST: return "RTA_DST";
        case RTA_SRC: return "RTA_SRC";
        case RTA_IIF: return "RTA_IIF";
        case RTA_OIF: return "RTA_OIF";
        case RTA_GATEWAY: return "RTA_GATEWAY";
        case RTA_PRIORITY: return "RTA_PRIORITY";
        case RTA_PREFSRC: return "RTA_PREFSRC";
        case RTA_METRICS: return "RTA_METRICS";
        case RTA_MULTIPATH: return "RTA_MULTIPATH";
        case RTA_FLOW: return "RTA_FLOW";
        case RTA_CACHEINFO: return "RTA_CACHEINFO";
        case RTA_TABLE: return "RTA_TABLE";
        case RTA_MARK: return "RTA_MARK";
        case RTA_PREF: return "RTA_PREF";
        case RTA_EXPIRES: return "RTA_EXPIRES";
        case RTA_UID: return "RTA_UID";
        default: return "other";
    }
}

static const char *type_name(unsigned char type) {
    switch (type) {
        case RTN_UNICAST: return "unicast";
        case RTN_LOCAL: return "local";
        case RTN_BROADCAST: return "broadcast";
        case RTN_MULTICAST: return "multicast";
        case RTN_UNREACHABLE: return "unreachable";
        default: return "other";
    }
}

static const char *protocol_name(unsigned char protocol) {
    switch (protocol) {
        case RTPROT_KERNEL: return "kernel";
        case RTPROT_BOOT: return "boot";
        case RTPROT_STATIC: return "static";
        case RTPROT_RA: return "ra";
        default: return "other";
    }
}

static const char *table_name(unsigned table) {
    if (table == RT_TABLE_LOCAL) return "local";
    if (table == RT_TABLE_MAIN) return "main";
    return "other";
}

/// Reads the dump of `family`'s routes into `routes`; 0, or -1 with what failed printed.
static int dump(int family) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        printf("socket: errno %d\n", errno);
        return -1;
    }
    struct {
        struct nlmsghdr header;
        struct rtmsg message;
    } request;
    memset(&request, 0, sizeof request);
    request.header.nlmsg_len = sizeof request;
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.message.rtm_family = (unsigned char)family;
    if (send(fd, &request, sizeof request, 0) < 0) {
        printf("send: errno %d\n", errno);
        close(fd);
        return -1;
    }
    static char buffer[65536];
    for (;;) {
        ssize_t got = recv(fd, buffer, sizeof buffer, 0);
        if (got < 0) {
            printf("recv: errno %d\n", errno);
            close(fd);
            return -1;
        }
        int remaining = (int)got;
        for (struct nlmsghdr *header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining)) {
            if (header->nlmsg_type == NLMSG_DONE) {
                close(fd);
                return 0;
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                printf("dump: error %d\n", ((struct nlmsgerr *)NLMSG_DATA(header))->error);
                close(fd);
                return -1;
            }
            if (header->nlmsg_type != RTM_NEWROUTE || route_count == MAX_ROUTES) continue;
            struct route *route = &routes[route_count++];
            memset(route, 0, sizeof *route);
            route->header = *(struct rtmsg *)NLMSG_DATA(header);
            route->table = route->header.rtm_table;
            int length = RTM_PAYLOAD(header);
            for (struct rtattr *attribute = RTM_RTA(NLMSG_DATA(header)); RTA_OK(attribute, length);
                 attribute = RTA_NEXT(attribute, length)) {
                if (route->attribute_count < MAX_ATTRIBUTES) {
                    route->attributes[route->attribute_count++] = attribute->rta_type;
                }
                size_t size = RTA_PAYLOAD(attribute);
                if (size > 16) size = 16;
                const void *data = RTA_DATA(attribute);
                switch (attribute->rta_type) {
                    case RTA_TABLE: memcpy(&route->table, data, 4); break;
                    case RTA_DST: memcpy(route->destination, data, size), route->has_destination = 1; break;
                    case RTA_GATEWAY: memcpy(route->gateway, data, size), route->has_gateway = 1; break;
                    case RTA_PREFSRC: memcpy(route->source, data, size), route->has_source = 1; break;
                    case RTA_OIF: memcpy(&route->oif, data, 4); break;
                    case RTA_PRIORITY: memcpy(&route->priority, data, 4), route->has_priority = 1; break;
                }
            }
        }
    }
}

static void print_route(const struct route *route) {
    char text[INET6_ADDRSTRLEN] = "default";
    int family = route->header.rtm_family;
    if (route->has_destination) inet_ntop(family, route->destination, text, sizeof text);
    printf("  %s %s/%d table %s proto %s scope %d tos %d flags 0x%x:", type_name(route->header.rtm_type), text,
           route->header.rtm_dst_len, table_name(route->table), protocol_name(route->header.rtm_protocol),
           route->header.rtm_scope, route->header.rtm_tos, route->header.rtm_flags);
    for (int i = 0; i < route->attribute_count; i++) printf(" %s", attribute_name(route->attributes[i]));
    if (route->has_source) {
        inet_ntop(family, route->source, text, sizeof text);
        printf(", src %s", text);
    }
    if (route->has_priority) printf(", metric %u", route->priority);
    printf("\n");
}

/// Whether a route of `family` matches: its type, table, destination (`length` bytes of `destination` and the prefix
/// `prefix`, none for a default route), interface, protocol, and, where asked, scope (-1: any), its address as its
/// source, a gateway, and its metric (-1: any).
static int has(int family, unsigned char type, unsigned table, const void *destination, int prefix, unsigned oif,
               unsigned char protocol, int scope, const void *source, int gateway, long metric) {
    int size = family == AF_INET ? 4 : 16;
    for (int i = 0; i < route_count; i++) {
        const struct route *route = &routes[i];
        if (route->header.rtm_family != family || route->header.rtm_type != type || route->table != table) continue;
        if (route->header.rtm_dst_len != prefix || route->oif != oif) continue;
        if (route->header.rtm_protocol != protocol) continue;
        if (destination ? !route->has_destination || memcmp(route->destination, destination, size) != 0
                        : route->has_destination) continue;
        if (scope >= 0 && route->header.rtm_scope != scope) continue;
        if (source && (!route->has_source || memcmp(route->source, source, size) != 0)) continue;
        if (gateway != route->has_gateway) continue;
        if (metric >= 0 && (!route->has_priority || route->priority != (unsigned long)metric)) continue;
        return 1;
    }
    return 0;
}

static void subnet_of(int family, const unsigned char *address, int prefix, unsigned char *subnet) {
    int size = family == AF_INET ? 4 : 16;
    for (int i = 0; i < size; i++) {
        int bits = prefix - 8 * i;
        subnet[i] = bits >= 8 ? address[i] : bits <= 0 ? 0 : (unsigned char)(address[i] & (0xff << (8 - bits)));
    }
}

static int prefix_of(const struct sockaddr *mask, int family) {
    const unsigned char *bytes = family == AF_INET
                                     ? (const unsigned char *)&((const struct sockaddr_in *)mask)->sin_addr
                                     : ((const struct sockaddr_in6 *)mask)->sin6_addr.s6_addr;
    int prefix = 0;
    for (int i = 0; i < (family == AF_INET ? 4 : 16); i++) prefix += __builtin_popcount(bytes[i]);
    return prefix;
}

static int counts(const struct ifaddrs *entry) {
    if (strcmp(entry->ifa_name, "lo") == 0 || !entry->ifa_addr) return 0;
    if (entry->ifa_addr->sa_family == AF_INET) return 1;
    if (entry->ifa_addr->sa_family != AF_INET6) return 0;
    const struct in6_addr *address = &((const struct sockaddr_in6 *)entry->ifa_addr)->sin6_addr;
    return !IN6_IS_ADDR_LINKLOCAL(address);
}

static int interface_counts(const struct ifaddrs *all, const char *name) {
    for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
        if (strcmp(entry->ifa_name, name) == 0 && counts(entry)) return 1;
    }
    return 0;
}

/// Whether every interface that counts with an IPv4 address has an IPv4 default route, and each with a global IPv6
/// address an IPv6 one.
static int ready(const struct ifaddrs *all) {
    for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
        if (!counts(entry)) continue;
        int family = entry->ifa_addr->sa_family, found = 0;
        unsigned index = if_nametoindex(entry->ifa_name);
        for (int i = 0; i < route_count && !found; i++) {
            found = routes[i].header.rtm_family == family && routes[i].header.rtm_dst_len == 0 &&
                    routes[i].oif == index && routes[i].header.rtm_type == RTN_UNICAST;
        }
        if (!found) return 0;
    }
    return 1;
}

static const char *yes(int value) {
    return value ? "yes" : "no";
}

int main(void) {
    struct ifaddrs *all;
    if (getifaddrs(&all) != 0) {
        printf("getifaddrs: errno %d\n", errno);
        return 1;
    }
    // Right after a boot netd may still be adding a network's routes: up to 30 s for its default routes.
    for (int second = 0;; second++) {
        route_count = 0;
        if (dump(AF_INET) != 0 || dump(AF_INET6) != 0) return 1;
        if (second == 30 || ready(all)) break;
        sleep(1);
    }
    unsigned lo = if_nametoindex("lo");

    printf("lo's IPv4 routes:\n");
    for (int i = 0; i < route_count; i++) {
        if (routes[i].header.rtm_family == AF_INET && routes[i].oif == lo) print_route(&routes[i]);
    }
    printf("lo's IPv6 routes:\n");
    for (int i = 0; i < route_count; i++) {
        if (routes[i].header.rtm_family == AF_INET6 && routes[i].oif == lo) print_route(&routes[i]);
    }

    // The routes of each address of an interface that counts, as Android's kernel and netd make them.
    int local4 = 1, broadcast4 = 1, main4 = 1, network4 = 1, local_network4 = 1, multicast4 = 1;
    int local6 = 1, multicast6 = 1, link_local6 = 1, prefix6 = 1, any4 = 0, any6 = 0;
    static const unsigned char multicast_subnet[4] = {224, 0, 0, 0}, multicast6_prefix[16] = {0xff},
                               link_local[16] = {0xfe, 0x80};
    for (const struct ifaddrs *entry = all; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || !entry->ifa_netmask || strcmp(entry->ifa_name, "lo") == 0) continue;
        if (!interface_counts(all, entry->ifa_name)) continue;
        unsigned index = if_nametoindex(entry->ifa_name), network = 1000 + index, local_network = 1000000000 + index;
        int family = entry->ifa_addr->sa_family;
        unsigned char subnet[16];
        if (family == AF_INET) {
            any4 = 1;
            const unsigned char *address = (const unsigned char *)&((struct sockaddr_in *)entry->ifa_addr)->sin_addr;
            int prefix = prefix_of(entry->ifa_netmask, AF_INET);
            // A point-to-point interface's prefix is its peer's (ifa_dstaddr, which shares ifa_broadaddr's place).
            const int point_to_point = (entry->ifa_flags & IFF_POINTOPOINT) && entry->ifa_dstaddr;
            const unsigned char *base =
                point_to_point ? (const unsigned char *)&((struct sockaddr_in *)entry->ifa_dstaddr)->sin_addr : address;
            subnet_of(AF_INET, base, prefix, subnet);
            local4 &= has(AF_INET, RTN_LOCAL, RT_TABLE_LOCAL, address, 32, index, RTPROT_KERNEL, RT_SCOPE_HOST, address,
                          0, -1);
            if ((entry->ifa_flags & IFF_BROADCAST) && entry->ifa_broadaddr && prefix < 31) {
                const void *broadcast = &((struct sockaddr_in *)entry->ifa_broadaddr)->sin_addr;
                broadcast4 &= has(AF_INET, RTN_BROADCAST, RT_TABLE_LOCAL, broadcast, 32, index, RTPROT_KERNEL,
                                  RT_SCOPE_LINK, address, 0, -1);
            }
            if (prefix == 32 && memcmp(subnet, address, 4) == 0) continue;  // no prefix route (fib_add_ifaddr)
            main4 &= has(AF_INET, RTN_UNICAST, RT_TABLE_MAIN, subnet, prefix, index, RTPROT_KERNEL, RT_SCOPE_LINK,
                         address, 0, -1);
            network4 &= has(AF_INET, RTN_UNICAST, network, subnet, prefix, index, RTPROT_STATIC, RT_SCOPE_LINK, NULL, 0,
                            -1);
            local_network4 &= has(AF_INET, RTN_UNICAST, local_network, subnet, prefix, index, RTPROT_STATIC,
                                  RT_SCOPE_LINK, NULL, 0, -1);
            multicast4 &= has(AF_INET, RTN_UNICAST, local_network, multicast_subnet, 24, index, RTPROT_STATIC,
                              RT_SCOPE_LINK, NULL, 0, -1);
        } else if (family == AF_INET6) {
            const unsigned char *address = ((struct sockaddr_in6 *)entry->ifa_addr)->sin6_addr.s6_addr;
            local6 &= has(AF_INET6, RTN_LOCAL, RT_TABLE_LOCAL, address, 128, index, RTPROT_KERNEL, -1, NULL, 0, 0);
            multicast6 &= has(AF_INET6, RTN_MULTICAST, RT_TABLE_LOCAL, multicast6_prefix, 8, index, RTPROT_KERNEL, -1,
                              NULL, 0, 256);
            link_local6 &= has(AF_INET6, RTN_UNICAST, network, link_local, 64, index, RTPROT_KERNEL, -1, NULL, 0, 256);
            if (!IN6_IS_ADDR_LINKLOCAL((const struct in6_addr *)address)) {
                any6 = 1;
                int prefix = prefix_of(entry->ifa_netmask, AF_INET6);
                subnet_of(AF_INET6, address, prefix, subnet);
                prefix6 &= has(AF_INET6, RTN_UNICAST, network, subnet, prefix, index, RTPROT_KERNEL, -1, NULL, 0, 256);
            }
        }
    }
    printf("IPv4 addresses: %s\n", yes(any4));
    printf("  each address's local route in table local, proto kernel, scope host, with it as src: %s\n", yes(local4));
    printf("  each broadcast address's route in table local, proto kernel, scope link: %s\n", yes(broadcast4));
    printf("  each subnet's route in table main, proto kernel, scope link, with its address as src: %s\n", yes(main4));
    printf("  each subnet's route in its network's table, 1000 + index, proto static, scope link: %s\n", yes(network4));
    printf("  each subnet's route in its local routes' table, 1000000000 + index: %s\n", yes(local_network4));
    printf("  224.0.0.0/24 in each local routes' table: %s\n", yes(multicast4));
    printf("IPv6 addresses beyond link-local: %s\n", yes(any6));
    printf("  each address's local route in table local, proto kernel, metric 0: %s\n", yes(local6));
    printf("  ff00::/8 multicast in table local for each interface, metric 256: %s\n", yes(multicast6));
    printf("  fe80::/64 in each network's table, proto kernel, metric 256: %s\n", yes(link_local6));
    printf("  each prefix's route in its network's table, proto kernel, metric 256: %s\n", yes(prefix6));

    // Default routes: each in the table of its interface's network, through a gateway on IPv4 (from the network's
    // DHCP or configuration, proto static) and a link-local router on IPv6 (from its advertisement, proto ra).
    int defaults4 = 0, gateway4 = 1, defaults6 = 0, router6 = 1;
    for (int i = 0; i < route_count; i++) {
        const struct route *route = &routes[i];
        if (route->header.rtm_dst_len != 0 || route->header.rtm_type != RTN_UNICAST) continue;
        char name[IF_NAMESIZE] = "";
        if (!if_indextoname(route->oif, name) || !interface_counts(all, name)) continue;
        if (route->header.rtm_family == AF_INET) {
            defaults4++;
            gateway4 &= route->table == 1000 + route->oif && route->has_gateway &&
                        route->header.rtm_protocol == RTPROT_STATIC && route->header.rtm_scope == RT_SCOPE_UNIVERSE;
        } else {
            defaults6++;
            router6 &= route->table == 1000 + route->oif && route->has_gateway && route->gateway[0] == 0xfe &&
                       (route->gateway[1] & 0xc0) == 0x80 && route->header.rtm_protocol == RTPROT_RA &&
                       route->has_priority && route->priority == 1024;
        }
    }
    printf("an IPv4 default route: %s\n", yes(defaults4 > 0));
    printf("  each in its network's table, through a gateway, proto static, scope universe: %s\n", yes(gateway4));
    printf("IPv6 default routes, each in its network's table, through a link-local router, proto ra, metric 1024: %s\n",
           yes(router6));
    freeifaddrs(all);
    return 0;
}
