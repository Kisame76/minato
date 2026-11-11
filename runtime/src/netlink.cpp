// The guest's network interfaces and NETLINK_ROUTE sockets (netlink.h, ADR-0054 step c), as Android 15's kernel
// answers an app: rtnetlink's address dump (devinet.c's and addrconf.c's fill functions), netlink_rcv_skb's walk and
// netlink_ack's acknowledgements, the ifreq ioctls of dev_ioctl and devinet_ioctl, and SELinux's checks per message
// (nlmsgtab.c) with system/sepolicy's rules for apps: no RTM_GETLINK, RTM_GETNEIGH or change, no ioctl on the socket.
// Written from their behavior; no kernel code is in it.
#include "netlink.h"

#include "syscalls.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <bit>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace apkrt::netlink {
namespace {

namespace linux_errno = sys::linux_errno;

constexpr int linux_inet = 2, linux_inet6 = 10;

// Linux's flags of an interface that Darwin's net/if.h has at the same bit; Darwin's IFF_MULTICAST is Linux's 0x1000.
// Darwin's IFF_SMART (which Linux's obsolete NOTRAILERS shares), OACTIVE, SIMPLEX and LINK bits mean nothing there.
constexpr unsigned shared_flags = IFF_UP | IFF_BROADCAST | IFF_DEBUG | IFF_LOOPBACK | IFF_POINTOPOINT | IFF_RUNNING |
                                  IFF_NOARP | IFF_PROMISC | IFF_ALLMULTI;
static_assert(IFF_UP == 0x1 && IFF_BROADCAST == 0x2 && IFF_DEBUG == 0x4 && IFF_LOOPBACK == 0x8 &&
              IFF_POINTOPOINT == 0x10 && IFF_RUNNING == 0x40 && IFF_NOARP == 0x80 && IFF_PROMISC == 0x100 &&
              IFF_ALLMULTI == 0x200);
constexpr unsigned linux_multicast = 0x1000;

/// lo as Linux has it: UP, LOOPBACK and RUNNING without MULTICAST, and loopback_setup's MTU.
constexpr unsigned lo_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
constexpr int lo_mtu = 65536;

// Netlink's numbers (include/uapi/linux/netlink.h, rtnetlink.h, if_addr.h).
constexpr std::uint16_t nlm_f_request = 1, nlm_f_multi = 2, nlm_f_ack = 4, nlm_f_dump = 0x300, nlm_f_capped = 0x100;
constexpr std::uint16_t nlmsg_error = 2, nlmsg_done = 3;
constexpr std::uint16_t rtm_base = 16, rtm_getlink = 18, rtm_newaddr = 20, rtm_getaddr = 22, rtm_getroute = 26,
                        rtm_getneigh = 30, rtm_getneightbl = 66, rtm_max = 123;
constexpr std::uint16_t ifa_address = 1, ifa_local = 2, ifa_label = 3, ifa_broadcast = 4, ifa_cacheinfo = 6,
                        ifa_flags = 8;
constexpr std::uint8_t ifa_f_permanent = 0x80;
constexpr std::uint8_t rt_scope_universe = 0, rt_scope_site = 200, rt_scope_link = 253, rt_scope_host = 254;
constexpr std::size_t header_size = 16;

/// What a dump's datagram holds at most: NLMSG_GOODSIZE on 4 KiB pages, the size of netlink_dump's first buffer.
constexpr std::size_t dump_room = 3776;

// --- The host's interfaces ---------------------------------------------------------------------------------------

std::string bytes_of(const sockaddr *address) {
    if (address == nullptr) return {};
    if (address->sa_family == AF_INET) {
        const auto &inet = *reinterpret_cast<const sockaddr_in *>(address);
        return std::string(reinterpret_cast<const char *>(&inet.sin_addr), 4);
    }
    if (address->sa_family == AF_INET6) {
        const auto &inet6 = *reinterpret_cast<const sockaddr_in6 *>(address);
        // A link-local address's bytes 2 and 3 are clear: Darwin's getifaddrs moves the scope KAME writes there
        // into sin6_scope_id itself [verified: macOS 27, every interface].
        return std::string(reinterpret_cast<const char *>(&inet6.sin6_addr), 16);
    }
    return {};
}

int prefix_of(const std::string &mask) {
    int bits = 0;
    for (const char each : mask) bits += std::popcount(static_cast<unsigned char>(each));
    return bits;
}

bool all_zero(const std::string &bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](char each) { return each == '\0'; });
}

/// The host's MTU of `name` where getifaddrs gave none.
int mtu_of(const std::string &name) {
    const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) return 0;
    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), sizeof request.ifr_name - 1);
    const int mtu = ::ioctl(probe, SIOCGIFMTU, &request) == 0 ? request.ifr_mtu : 0;
    ::close(probe);
    return mtu;
}

bool starts_with_number(const std::string &name, std::string_view prefix) {
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
           std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                       [](char each) { return each >= '0' && each <= '9'; });
}

bool link_local(const Address &address) {
    const auto first = static_cast<unsigned char>(address.bytes[0]);
    const auto second = static_cast<unsigned char>(address.bytes[1]);
    if (address.family == linux_inet) return first == 169 && second == 254;
    return first == 0xfe && (second & 0xc0) == 0x80;
}

/// Whether an interface has an address that is not link-local, as a VPN's utun and a USB adapter's en have.
bool routable(const HostInterface &interface) {
    return std::any_of(interface.addresses.begin(), interface.addresses.end(),
                       [](const Address &each) { return !link_local(each); });
}

// --- The kernel's bytes ------------------------------------------------------------------------------------------

void put16(std::string &out, std::uint16_t value) {
    out.append(reinterpret_cast<const char *>(&value), 2);
}

void put32(std::string &out, std::uint32_t value) {
    out.append(reinterpret_cast<const char *>(&value), 4);
}

std::uint16_t read16(std::string_view bytes, std::size_t at) {
    std::uint16_t value;
    std::memcpy(&value, bytes.data() + at, 2);
    return value;
}

std::uint32_t read32(std::string_view bytes, std::size_t at) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

void attribute(std::string &out, std::uint16_t type, std::string_view payload) {
    put16(out, static_cast<std::uint16_t>(4 + payload.size()));
    put16(out, type);
    out.append(payload);
    out.resize((out.size() + 3) & ~std::size_t{3}, '\0');
}

std::string message(std::uint16_t type, std::uint16_t flags, std::uint32_t sequence, std::uint32_t port,
                    std::string_view payload) {
    std::string out;
    put32(out, static_cast<std::uint32_t>(header_size + payload.size()));
    put16(out, type);
    put16(out, flags);
    put32(out, sequence);
    put32(out, port);
    out.append(payload);
    return out;
}

std::uint8_t scope_of(const Address &address) {
    const auto first = static_cast<unsigned char>(address.bytes[0]);
    const auto second = static_cast<unsigned char>(address.bytes[1]);
    if (address.family == linux_inet) return first == 127 ? rt_scope_host : rt_scope_universe;
    static const std::string loopback = std::string(15, '\0') + '\1';
    if (address.bytes == loopback) return rt_scope_host;
    if (first == 0xfe && (second & 0xc0) == 0x80) return rt_scope_link;
    if (first == 0xfe && (second & 0xc0) == 0xc0) return rt_scope_site;
    return rt_scope_universe;
}

/// RTM_NEWADDR for `address` of `interface`, in the order of inet_fill_ifaddr's attributes for IPv4 and of
/// inet6_fill_ifaddr's for IPv6. Every address is permanent, with lifetimes that never end and creation and update
/// times of 0: Darwin keeps no lease that Linux's would show.
std::string new_address(const Interface &interface, const Address &address, std::uint32_t sequence,
                        std::uint32_t port) {
    std::string payload;
    payload += static_cast<char>(address.family);
    payload += static_cast<char>(address.prefix);
    payload += static_cast<char>(ifa_f_permanent);
    payload += static_cast<char>(scope_of(address));
    put32(payload, interface.index);
    std::string flags, cacheinfo;
    put32(flags, ifa_f_permanent);
    put32(cacheinfo, 0xffffffff);  // ifa_prefered: INFINITY_LIFE_TIME
    put32(cacheinfo, 0xffffffff);  // ifa_valid
    put32(cacheinfo, 0);           // cstamp
    put32(cacheinfo, 0);           // tstamp
    if (address.family == linux_inet) {
        attribute(payload, ifa_address, address.peer.empty() ? address.bytes : address.peer);
        attribute(payload, ifa_local, address.bytes);
        if (!address.broadcast.empty() && !all_zero(address.broadcast)) {
            attribute(payload, ifa_broadcast, address.broadcast);
        }
        attribute(payload, ifa_label, interface.name + '\0');
        attribute(payload, ifa_flags, flags);
        attribute(payload, ifa_cacheinfo, cacheinfo);
    } else {
        if (!address.peer.empty()) {
            attribute(payload, ifa_local, address.bytes);
            attribute(payload, ifa_address, address.peer);
        } else {
            attribute(payload, ifa_address, address.bytes);
        }
        attribute(payload, ifa_cacheinfo, cacheinfo);
        attribute(payload, ifa_flags, flags);
    }
    return message(rtm_newaddr, nlm_f_multi, sequence, port, payload);
}

std::string done(std::uint32_t sequence, std::uint32_t port) {
    std::string error;
    put32(error, 0);
    return message(nlmsg_done, nlm_f_multi, sequence, port, error);
}

/// RTM_GETADDR's dump for `family` (another than AF_INET and AF_INET6 is both, rtnl_dump_all): IPv4's addresses in
/// datagrams of their own, then IPv6's, each family in the order of the interfaces, then NLMSG_DONE alone.
void address_dump(const std::vector<Interface> &interfaces, int family, std::uint32_t sequence, std::uint32_t port,
                  std::vector<std::string> &replies) {
    for (const int each : {linux_inet, linux_inet6}) {
        if ((family == linux_inet || family == linux_inet6) && family != each) continue;
        std::string datagram;
        for (const auto &interface : interfaces) {
            for (const auto &address : interface.addresses) {
                if (address.family != each) continue;
                std::string one = new_address(interface, address, sequence, port);
                if (!datagram.empty() && datagram.size() + one.size() > dump_room) {
                    replies.push_back(std::move(datagram));
                    datagram.clear();
                }
                datagram += one;
            }
        }
        if (!datagram.empty()) replies.push_back(std::move(datagram));
    }
    replies.push_back(done(sequence, port));
}

// --- The routes (ADR-0054 step c3) ------------------------------------------------------------------------------
// Android 15's tables as an app dumps them [verified: the emulator, route-test and `ip route show table all`]: the
// kernel's local table (255) with each address's local route and each subnet's broadcast, its main table (254) with
// each subnet, and netd's (RouteController.cpp) table of each network, 1000 + its interface's index, with its subnets,
// its link-local and prefix routes (the kernel's beside netd's own) and its default routes, and of its local routes,
// 1000000000 + the index, with its subnets and IPv4's link-local multicast. Only the default routes come from the
// host; the rest follows from the guest's addresses as Linux's fib_add_ifaddr and addrconf make it.

constexpr std::uint16_t rtm_newroute = 24;
constexpr std::uint16_t rta_dst = 1, rta_oif = 4, rta_gateway = 5, rta_priority = 6, rta_prefsrc = 7,
                        rta_metrics = 8, rta_cacheinfo = 12, rta_table = 15, rta_pref = 20, rtax_hoplimit = 10;
constexpr std::uint8_t rtn_unicast = 1, rtn_local = 2, rtn_broadcast = 3, rtn_multicast = 5;
constexpr std::uint8_t rtprot_kernel = 2, rtprot_static = 4, rtprot_ra = 9;
constexpr std::uint32_t table_main = 254, table_local = 255, table_compat = 252, network_table = 1000,
                        local_network_table = 1'000'000'000;
// IPv6's metrics: an address's local route, the kernel's prefix routes (IP6_RT_PRIO_ADDRCONF), netd's routes and an
// advertised router's (IP6_RT_PRIO_USER), and the hop limit an advertisement gives (the emulator's router: 64).
constexpr std::uint32_t local_metric = 0, kernel_metric = 256, netd_metric = 1024, router_hop_limit = 64;

struct Route {
    int family = linux_inet;
    std::uint32_t table = table_main;
    std::uint8_t type = rtn_unicast, protocol = rtprot_kernel, scope = rt_scope_universe;
    std::string destination;  // the prefix's bytes, empty for a default route
    int prefix = 0;
    unsigned oif = 0;
    std::string source, gateway;
    std::uint32_t metric = 0;  // IPv6's
    bool hop_limit = false;    // IPv6: RTAX_HOPLIMIT, as an advertised router's route has it
};

/// `bytes` with every bit beyond the first `prefix` cleared, or, with `ones`, set.
std::string masked(std::string bytes, int prefix, bool ones = false) {
    for (std::size_t i = 0; i < bytes.size(); i++) {
        const int bits = prefix - 8 * static_cast<int>(i);
        if (bits >= 8) continue;
        const unsigned kept = bits <= 0 ? 0 : 0xffu << (8 - bits) & 0xffu;
        const auto byte = static_cast<unsigned char>(bytes[i]);
        bytes[i] = static_cast<char>(ones ? (byte & kept) | (~kept & 0xffu) : byte & kept);
    }
    return bytes;
}

/// The routes Android 15 has for `interfaces` of `family`, with `defaults`' default routes for them.
std::vector<Route> routes_of(const std::vector<Interface> &interfaces, const std::vector<HostRoute> &defaults,
                             int family) {
    std::vector<Route> out;
    std::set<std::tuple<std::uint32_t, std::string, int, std::uint8_t, unsigned, std::uint32_t, std::uint8_t>> seen;
    const auto add = [&](Route route) {
        route.family = family;
        if (seen.emplace(route.table, route.destination, route.prefix, route.type, route.oif, route.metric,
                         route.protocol).second) {
            out.push_back(std::move(route));
        }
    };
    for (const auto &interface : interfaces) {
        const bool loopback = (interface.flags & IFF_LOOPBACK) != 0;
        const unsigned index = interface.index;
        const std::uint32_t network = network_table + index, local_network = local_network_table + index;
        bool any = false;
        for (const auto &address : interface.addresses) {
            if (address.family != family) continue;
            any = true;
            const std::string &bytes = address.bytes;
            if (family == linux_inet) {
                const int prefix = std::clamp(address.prefix, 0, 32);
                const std::string subnet = masked(address.peer.empty() ? bytes : address.peer, prefix);
                add({.table = table_local, .type = rtn_local, .scope = rt_scope_host, .destination = bytes,
                     .prefix = 32, .oif = index, .source = bytes});
                // fib_add_ifaddr: a prefix route where the prefix is not the address, a point-to-point peer's too.
                if ((prefix < 32 || subnet != bytes) && !all_zero(subnet)) {
                    if (loopback) {
                        add({.table = table_local, .type = rtn_local, .scope = rt_scope_host, .destination = subnet,
                             .prefix = prefix, .oif = index, .source = bytes});
                    } else {
                        add({.table = table_main, .scope = rt_scope_link, .destination = subnet, .prefix = prefix,
                             .oif = index, .source = bytes});
                        for (const std::uint32_t table : {network, local_network}) {
                            add({.table = table, .protocol = rtprot_static, .scope = rt_scope_link,
                                 .destination = subnet, .prefix = prefix, .oif = index});
                        }
                    }
                    if (prefix < 31) {
                        add({.table = table_local, .type = rtn_broadcast, .scope = rt_scope_link,
                             .destination = masked(subnet, prefix, true), .prefix = 32, .oif = index,
                             .source = bytes});
                    }
                }
            } else {
                add({.table = table_local, .type = rtn_local, .destination = bytes, .prefix = 128, .oif = index,
                     .metric = local_metric});
                if (loopback || address.prefix >= 128 || (address.prefix <= 0 && !link_local(address))) continue;
                const int prefix = link_local(address) ? 64 : address.prefix;
                const std::string subnet = masked(bytes, prefix);
                add({.table = network, .destination = subnet, .prefix = prefix, .oif = index,
                     .metric = kernel_metric});
                for (const std::uint32_t table : {network, local_network}) {
                    add({.table = table, .protocol = rtprot_static, .destination = subnet, .prefix = prefix,
                         .oif = index, .metric = netd_metric});
                }
            }
        }
        if (!any || loopback) continue;
        if (family == linux_inet) {
            add({.table = local_network, .protocol = rtprot_static, .scope = rt_scope_link,
                 .destination = std::string("\xe0\0\0\0", 4), .prefix = 24, .oif = index});
        } else {
            add({.table = table_local, .type = rtn_multicast,
                 .destination = std::string("\xff") + std::string(15, '\0'), .prefix = 8, .oif = index,
                 .metric = kernel_metric});
        }
        // A VPN's routes go over its tunnel without a gateway whatever router the host's route names: they come from
        // VpnService.Builder.addRoute(prefix), a RouteInfo with no gateway, and netd adds them as such (proto static,
        // IPv4 scope link, IPv6 with no advertisement's hop limit) [verified: VpnService and Vpn.java, Android 15].
        const bool tunnel = interface.name.compare(0, 3, "tun") == 0;
        for (const auto &route : defaults) {
            if (route.family != family || route.index != index) continue;
            const std::string &via = tunnel ? std::string() : route.gateway;
            const bool router = !via.empty();
            if (family == linux_inet) {
                add({.table = network, .protocol = rtprot_static, .scope = router ? rt_scope_universe : rt_scope_link,
                     .oif = index, .gateway = via});
            } else {
                add({.table = network, .protocol = router ? rtprot_ra : rtprot_static, .oif = index, .gateway = via,
                     .metric = netd_metric, .hop_limit = router});
            }
        }
    }
    // The order of a dump: the tables by the kernel's hash of their IDs (fib_table_hash, 256 buckets), each by its
    // prefixes' addresses (fib_trie's walk); IPv6's tree gives a table's default route last, and its routes to one
    // prefix by their metrics.
    const auto key = [family](const Route &route) {
        const bool last = family == linux_inet6 && route.prefix == 0;
        const std::string address = route.destination.empty() ? std::string(family == linux_inet ? 4 : 16, '\0')
                                                              : route.destination;
        return std::tuple(route.table & 255, route.table, last, address, route.prefix, route.metric, route.oif);
    };
    std::stable_sort(out.begin(), out.end(), [&](const Route &a, const Route &b) { return key(a) < key(b); });
    return out;
}

/// RTM_NEWROUTE for `route`, in the order of fib_dump_info's attributes for IPv4 (RTA_TABLE, RTA_DST, RTA_PREFSRC,
/// then the next hop's RTA_GATEWAY and RTA_OIF) and of rt6_fill_node's for IPv6 (RTA_TABLE, RTA_DST, RTA_METRICS,
/// RTA_PRIORITY, RTA_GATEWAY, RTA_OIF, RTA_CACHEINFO and RTA_PREF); a table beyond 255 is RT_TABLE_COMPAT in the
/// header. No route expires, and no cache entry is shown.
std::string new_route(const Route &route, std::uint32_t sequence, std::uint32_t port) {
    std::string payload;
    payload += static_cast<char>(route.family);
    payload += static_cast<char>(route.prefix);
    payload += '\0';  // rtm_src_len
    payload += '\0';  // rtm_tos
    payload += static_cast<char>(route.table < 256 ? route.table : table_compat);
    payload += static_cast<char>(route.protocol);
    payload += static_cast<char>(route.scope);
    payload += static_cast<char>(route.type);
    put32(payload, 0);  // rtm_flags
    std::string number;
    put32(number, route.table);
    attribute(payload, rta_table, number);
    if (route.prefix > 0) attribute(payload, rta_dst, route.destination);
    if (route.family == linux_inet) {
        if (!route.source.empty()) attribute(payload, rta_prefsrc, route.source);
    } else {
        if (route.hop_limit) {
            std::string metrics, limit;
            put32(limit, router_hop_limit);
            attribute(metrics, rtax_hoplimit, limit);
            attribute(payload, rta_metrics, metrics);
        }
        std::string metric;
        put32(metric, route.metric);
        attribute(payload, rta_priority, metric);
    }
    if (!route.gateway.empty()) attribute(payload, rta_gateway, route.gateway);
    std::string oif;
    put32(oif, route.oif);
    attribute(payload, rta_oif, oif);
    if (route.family == linux_inet6) {
        attribute(payload, rta_cacheinfo, std::string(32, '\0'));
        attribute(payload, rta_pref, std::string(1, '\0'));  // ICMPV6_ROUTER_PREF_MEDIUM
    }
    return message(rtm_newroute, nlm_f_multi, sequence, port, payload);
}

/// RTM_GETROUTE's dump for `family` (another than AF_INET and AF_INET6 is both, rtnl_dump_all): IPv4's routes in
/// datagrams of their own, then IPv6's, then NLMSG_DONE alone, as the address dump.
void route_dump(const std::vector<Interface> &interfaces, const std::vector<HostRoute> &defaults, int family,
                std::uint32_t sequence, std::uint32_t port, std::vector<std::string> &replies) {
    for (const int each : {linux_inet, linux_inet6}) {
        if ((family == linux_inet || family == linux_inet6) && family != each) continue;
        std::string datagram;
        for (const auto &route : routes_of(interfaces, defaults, each)) {
            std::string one = new_route(route, sequence, port);
            if (!datagram.empty() && datagram.size() + one.size() > dump_room) {
                replies.push_back(std::move(datagram));
                datagram.clear();
            }
            datagram += one;
        }
        if (!datagram.empty()) replies.push_back(std::move(datagram));
    }
    replies.push_back(done(sequence, port));
}

/// netlink_ack: NLMSG_ERROR with `error` for `original`, which it repeats whole for an error and only its header for
/// an acknowledgement (NLM_F_CAPPED), as without NETLINK_CAP_ACK and NETLINK_EXT_ACK.
std::string acknowledgement(long error, std::string_view original, std::uint32_t port) {
    std::string payload;
    put32(payload, static_cast<std::uint32_t>(static_cast<std::int32_t>(error)));
    payload.append(error == 0 ? original.substr(0, header_size) : original);
    return message(nlmsg_error, error == 0 ? nlm_f_capped : 0, read32(original, 8), port, payload);
}

/// SELinux's answer for a message of `type` from an app: what nlmsgtab.c maps it to and the app's domain may not do
/// (net.te grants netdomain nlmsg_read alone; untrusted apps never get nlmsg_readpriv or, past API 30,
/// nlmsg_getneigh). A type it maps to nothing is refused, as Android's policy denies the unknown.
long permission(std::uint16_t type) {
    if (type < rtm_base || type > rtm_max) return -linux_errno::einval;
    if ((type & 3) != 2) return -linux_errno::eacces;  // NEW, DEL and SET: nlmsg_write
    if (type == rtm_getlink || type == rtm_getneigh || type == rtm_getneightbl) return -linux_errno::eacces;
    return 0;
}

/// NLMSG_ALIGN in size_t, where 32 bits would wrap near their end.
std::size_t aligned(std::uint32_t length) {
    return (std::size_t{length} + 3) & ~std::size_t{3};
}

/// A dump started: no acknowledgement follows, as netlink_rcv_skb skips it after netlink_dump_start.
constexpr long dumped = 1;

/// rtnetlink_rcv_msg's answer for a request to read: a dump, or an error for a request for one object.
long respond(std::string_view request, std::uint32_t port, const std::function<std::vector<Interface>()> &interfaces,
             const std::function<std::vector<HostRoute>()> &routes, std::vector<std::string> &replies) {
    if (request.size() < header_size + 1) return 0;  // shorter than rtgenmsg
    const auto type = read16(request, 4), flags = read16(request, 6);
    const auto sequence = read32(request, 8);
    const int family = static_cast<unsigned char>(request[header_size]);
    const bool dump = (flags & nlm_f_dump) == nlm_f_dump;
    if (type == rtm_getaddr) {
        if (!dump) return -linux_errno::eopnotsupp;
        address_dump(interfaces(), family, sequence, port, replies);
        return dumped;
    }
    if (type == rtm_getroute && dump) {
        route_dump(interfaces(), routes(), family, sequence, port, replies);
        return dumped;
    }
    if (dump) {
        replies.push_back(done(sequence, port));  // tables the guest has none of
        return dumped;
    }
    return type == rtm_getroute ? -linux_errno::enetunreach : -linux_errno::eopnotsupp;
}

// --- The ioctls --------------------------------------------------------------------------------------------------

constexpr std::uint32_t siocgifname = 0x8910, siocgifconf = 0x8912, siocgifflags = 0x8913, siocgifaddr = 0x8915,
                        siocgifdstaddr = 0x8917, siocgifbrdaddr = 0x8919, siocgifnetmask = 0x891b,
                        siocgifmtu = 0x8921, siocgifhwaddr = 0x8927, siocgifindex = 0x8933;

/// Linux's sockaddr_in.
struct LinuxInetName {
    std::uint16_t family, port;
    std::uint32_t address;
    unsigned char zero[8];
};

/// Linux's struct ifreq on arm64: IFNAMSIZ bytes of name, then a union of 24 bytes.
struct LinuxIfreq {
    char name[16];
    union {
        LinuxInetName inet;
        std::int16_t flags;
        std::int32_t value;
        unsigned char raw[24];
    };
};
static_assert(sizeof(LinuxIfreq) == 40);

struct LinuxIfconf {
    std::int32_t length, pad;
    std::uint64_t buffer;
};
static_assert(sizeof(LinuxIfconf) == 16);

/// The interface `name` names, as dev_ioctl finds it: what comes before a colon, an alias's label.
const Interface *by_name(const std::vector<Interface> &all, const char *name) {
    std::string wanted(name, strnlen(name, 15));
    if (const auto colon = wanted.find(':'); colon != std::string::npos) wanted.resize(colon);
    for (const auto &each : all) {
        if (each.name == wanted) return &each;
    }
    return nullptr;
}

std::uint32_t mask_of(int prefix) {
    return prefix <= 0 ? 0 : htonl(prefix >= 32 ? 0xffffffffu : ~(0xffffffffu >> prefix));
}

std::uint32_t inet_of(const std::string &bytes) {
    std::uint32_t value = 0;
    if (bytes.size() == 4) std::memcpy(&value, bytes.data(), 4);
    return value;
}

/// devinet_ioctl's getters: the address whose label is the whole name, the one the request names first.
long inet_getter(std::uint32_t request, LinuxIfreq &ifr) {
    const auto all = interfaces();
    const Interface *interface = by_name(all, ifr.name);
    if (interface == nullptr) return -linux_errno::enodev;
    const std::string label(ifr.name, strnlen(ifr.name, 15));
    const bool match_address = ifr.inet.family == AF_INET;
    const std::uint32_t named = ifr.inet.address;
    const Address *found = nullptr;
    if (label == interface->name) {
        for (const auto &each : interface->addresses) {
            if (each.family != linux_inet) continue;
            if (match_address && inet_of(each.bytes) == named) {
                found = &each;
                break;
            }
            if (found == nullptr) found = &each;
        }
    }
    if (found == nullptr) return -linux_errno::eaddrnotavail;
    std::uint32_t value = 0;
    switch (request) {
        case siocgifaddr: value = inet_of(found->bytes); break;
        case siocgifbrdaddr: value = inet_of(found->broadcast); break;
        case siocgifdstaddr: value = inet_of(found->peer.empty() ? found->bytes : found->peer); break;
        default: value = mask_of(found->prefix); break;  // SIOCGIFNETMASK
    }
    std::memset(ifr.raw, 0, sizeof ifr.raw);
    ifr.inet.family = linux_inet;
    ifr.inet.address = value;
    return 0;
}

/// dev_ifconf on the guest's struct ifconf at `address`, read once: an ifreq for each IPv4 address, its interface's
/// name and the address, as many as fit; with no buffer the length they need. EFAULT where the guest's memory cannot
/// be read or written.
long interface_list(std::uint64_t address) {
    LinuxIfconf conf;
    if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&conf), address, sizeof conf)) {
        return -linux_errno::efault;
    }
    constexpr std::int32_t size = sizeof(LinuxIfreq);
    std::int32_t total = 0;
    for (const auto &interface : interfaces()) {
        for (const auto &one : interface.addresses) {
            if (one.family != linux_inet) continue;
            if (conf.buffer == 0) {
                total += size;
                continue;
            }
            if (conf.length - total < size) break;
            LinuxIfreq entry{};
            std::memset(entry.raw, 0, sizeof entry.raw);
            std::strncpy(entry.name, interface.name.c_str(), sizeof entry.name - 1);
            entry.inet.family = linux_inet;
            entry.inet.address = inet_of(one.bytes);
            if (!sys::memory::copy_checked(conf.buffer + static_cast<std::uint64_t>(total),
                                           reinterpret_cast<std::uint64_t>(&entry), size)) {
                return -linux_errno::efault;
            }
            total += size;
        }
    }
    if (!sys::memory::copy_checked(address + offsetof(LinuxIfconf, length), reinterpret_cast<std::uint64_t>(&total),
                                   sizeof total)) {
        return -linux_errno::efault;
    }
    return 0;
}

}  // namespace

std::vector<Interface> guest_interfaces(const std::vector<HostInterface> &host) {
    std::vector<const HostInterface *> sorted;
    for (const auto &each : host) sorted.push_back(&each);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const HostInterface *one, const HostInterface *other) { return one->index < other->index; });
    std::vector<Interface> guest;
    int tunnels = 0, wired = 0;
    for (const HostInterface *each : sorted) {
        Interface interface;
        interface.host_name = each->name;
        interface.index = each->index;
        interface.flags = (each->flags & shared_flags) | ((each->flags & IFF_MULTICAST) != 0 ? linux_multicast : 0);
        interface.mtu = each->mtu;
        interface.addresses = each->addresses;
        if (each->name == "lo0") {
            interface.name = "lo";
            interface.index = 1;
            interface.flags = lo_flags;
            interface.mtu = lo_mtu;
            Address inet, inet6;
            inet.family = linux_inet;
            inet.bytes = std::string("\x7f\0\0\x01", 4);
            inet.prefix = 8;
            inet6.family = linux_inet6;
            inet6.bytes = std::string(15, '\0') + '\1';
            inet6.prefix = 128;
            interface.addresses = {inet, inet6};
        } else if (each->name == "en0") {
            interface.name = "wlan0";
        } else if (starts_with_number(each->name, "pdp_ip") && !each->addresses.empty()) {
            interface.name = "rmnet_data" + each->name.substr(6);
        } else if (starts_with_number(each->name, "utun") && routable(*each)) {
            interface.name = "tun" + std::to_string(tunnels++);
        } else if (starts_with_number(each->name, "en") && routable(*each)) {
            interface.name = "eth" + std::to_string(wired++);
        } else {
            continue;  // awdl, llw, anpi, ipsec, bridge, gif, stf, an unaddressed utun or pdp_ip: no Android name
        }
        guest.push_back(std::move(interface));
    }
    return guest;
}

// A fake host network for the Mac session and tests: there is no NWPathMonitor on the Mac, so a fake lets the guest's
// getifaddrs/netlink AND the LinkProperties snapshot read one fixed source. Set once at session start
// (fake_path_monitor.cpp, under MINATO_FAKE_NETWORK) or by a test; never on the device (the real monitor runs).
namespace {
bool fake_host_set = false;
std::vector<HostInterface> fake_host_interfaces;
std::vector<HostRoute> fake_host_routes;
}  // namespace

void set_fake_host(std::vector<HostInterface> interfaces, std::vector<HostRoute> routes) {
    fake_host_interfaces = std::move(interfaces);
    fake_host_routes = std::move(routes);
    fake_host_set = true;
}

void clear_fake_host() {
    fake_host_set = false;
    fake_host_interfaces.clear();
    fake_host_routes.clear();
}

std::vector<HostInterface> host_interfaces() {
    if (fake_host_set) return fake_host_interfaces;
    ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0) return {};
    std::map<std::string, HostInterface> by_host_name;
    for (const ifaddrs *each = list; each != nullptr; each = each->ifa_next) {
        if (each->ifa_name == nullptr) continue;
        HostInterface &interface = by_host_name[each->ifa_name];
        if (interface.name.empty()) interface.name = each->ifa_name;
        interface.flags = each->ifa_flags;
        const sockaddr *address = each->ifa_addr;
        if (address == nullptr) continue;
        if (address->sa_family == AF_LINK) {
            interface.index = reinterpret_cast<const sockaddr_dl *>(address)->sdl_index;
            const auto *data = static_cast<const if_data *>(each->ifa_data);
            if (data != nullptr) interface.mtu = static_cast<int>(data->ifi_mtu);
            continue;
        }
        if (address->sa_family != AF_INET && address->sa_family != AF_INET6) continue;
        Address one;
        one.family = address->sa_family == AF_INET ? linux_inet : linux_inet6;
        one.bytes = bytes_of(address);
        one.prefix = prefix_of(bytes_of(each->ifa_netmask));
        // Darwin's ifa_dstaddr is a point-to-point interface's peer, and a broadcast interface's broadcast address.
        const std::string other = bytes_of(each->ifa_dstaddr);
        if (!other.empty() && !all_zero(other)) {
            if ((each->ifa_flags & IFF_POINTOPOINT) != 0) {
                one.peer = other;
            } else if ((each->ifa_flags & IFF_BROADCAST) != 0 && one.family == linux_inet) {
                one.broadcast = other;
            }
        }
        interface.addresses.push_back(std::move(one));
    }
    ::freeifaddrs(list);
    std::vector<HostInterface> result;
    for (auto &[name, interface] : by_host_name) {
        if (interface.index == 0) interface.index = ::if_nametoindex(name.c_str());
        if (interface.mtu == 0) interface.mtu = mtu_of(name);
        result.push_back(std::move(interface));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const HostInterface &one, const HostInterface &other) { return one.index < other.index; });
    return result;
}

std::vector<Interface> interfaces() {
    return guest_interfaces(host_interfaces());
}

// Darwin's routing messages (net/route.h, which the iOS SDK does not ship): rt_msghdr is 92 bytes, its length at 0,
// version at 2, interface index at 4, flags at 8 and the bits of the sockaddrs that follow at 12, each padded to 4
// bytes (ROUNDUP32, 4 for an empty one).
namespace {
constexpr std::size_t routing_header = 92;
constexpr int rtm_version = 5, rtf_up = 0x1, rtf_gateway = 0x2, rtf_host = 0x4, rtf_reject = 0x8,
              rtf_llinfo = 0x400, rtf_blackhole = 0x1000, rtf_wascloned = 0x20000, rtax_dst = 0, rtax_gateway = 1,
              rtax_netmask = 2, rtax_max = 8, net_rt_dump = 1;
constexpr std::size_t dump_cap = std::size_t{16} << 20;
}  // namespace

std::vector<HostRoute> default_routes(std::string_view dump) {
    std::vector<HostRoute> found;
    std::set<std::pair<int, unsigned>> seen;
    for (std::size_t at = 0; dump.size() - at >= routing_header;) {
        const std::size_t length = read16(dump, at);
        if (length < routing_header || length > dump.size() - at) break;
        const std::string_view one = dump.substr(at, length);
        at += length;
        if (static_cast<unsigned char>(one[2]) != rtm_version) continue;
        const auto flags = static_cast<int>(read32(one, 8)), present = static_cast<int>(read32(one, 12));
        // Not up, a host route, a VPN's reject or blackhole, an ARP or neighbour entry, a clone.
        constexpr int no_default = rtf_host | rtf_reject | rtf_llinfo | rtf_blackhole | rtf_wascloned;
        if ((flags & rtf_up) == 0 || (flags & no_default) != 0) continue;
        std::string_view addresses[rtax_max];
        bool fits = true;
        for (std::size_t i = 0, offset = routing_header; i < rtax_max && fits; i++) {
            if ((present & (1 << i)) == 0) continue;
            fits = offset < length;
            if (!fits) break;
            const std::size_t size = static_cast<unsigned char>(one[offset]);
            fits = size <= length - offset;
            addresses[i] = one.substr(offset, std::min(size, length - offset));
            offset += size == 0 ? 4 : (size + 3) & ~std::size_t{3};
        }
        if (!fits) continue;
        const std::string_view destination = addresses[rtax_dst];
        if (destination.size() < 2) continue;
        const int family = static_cast<unsigned char>(destination[1]);
        if (family != AF_INET && family != AF_INET6) continue;
        const std::size_t start = family == AF_INET ? 4 : 8, width = family == AF_INET ? 4 : 16;
        const auto zero = [](std::string_view bytes) {
            return std::all_of(bytes.begin(), bytes.end(), [](char each) { return each == '\0'; });
        };
        if (destination.size() < start + width || !zero(destination.substr(start, width))) continue;
        const std::string_view netmask = addresses[rtax_netmask];
        if (netmask.size() > start && !zero(netmask.substr(start))) continue;
        HostRoute route{family == AF_INET ? linux_inet : linux_inet6, read16(one, 4), {}};
        const std::string_view gateway = addresses[rtax_gateway];
        if ((flags & rtf_gateway) != 0) {
            if (gateway.size() < start + width || static_cast<unsigned char>(gateway[1]) != family) continue;
            route.gateway = std::string(gateway.substr(start, width));
            if (family == AF_INET6 && static_cast<unsigned char>(route.gateway[0]) == 0xfe &&
                (static_cast<unsigned char>(route.gateway[1]) & 0xc0) == 0x80) {
                route.gateway[2] = route.gateway[3] = '\0';  // KAME's scope
            }
        }  // without RTF_GATEWAY the route goes over the interface's link, whatever its gateway names
        if (seen.emplace(route.family, route.index).second) found.push_back(std::move(route));
    }
    return found;
}

std::vector<HostRoute> host_default_routes() {
    if (fake_host_set) return fake_host_routes;
    std::vector<HostRoute> all;
    for (const int family : {AF_INET, AF_INET6}) {
        int names[6] = {CTL_NET, PF_ROUTE, 0, family, net_rt_dump, 0};
        std::string dump;
        bool read = false;
        for (int attempt = 0; attempt < 3 && !read; attempt++) {  // the table can grow between the two calls
            std::size_t size = 0;
            if (::sysctl(names, 6, nullptr, &size, nullptr, 0) != 0) break;
            if (size > dump_cap) {
                errno = EFBIG;
                break;
            }
            dump.resize(size + size / 4 + 1024);
            size = dump.size();
            read = ::sysctl(names, 6, dump.data(), &size, nullptr, 0) == 0;
            dump.resize(read ? size : 0);
            if (!read && errno != ENOMEM) break;
        }
        if (!read) {  // the sandbox, or a table that keeps growing: no default routes for the family
            static std::once_flag reported[2];
            std::call_once(reported[family == AF_INET ? 0 : 1], [family, error = errno] {
                char message[128];
                std::snprintf(message, sizeof message, "apkrt: the host's IPv%d routes cannot be read (%s)\n",
                              family == AF_INET ? 4 : 6, std::strerror(error));
                sys::report(message);
            });
        }
        for (auto &each : default_routes(dump)) all.push_back(std::move(each));
    }
    return all;
}

std::string kernel_name() {
    std::string name;
    put16(name, 16);  // AF_NETLINK
    put16(name, 0);
    put32(name, 0);   // nl_pid: the kernel
    put32(name, 0);   // nl_groups
    return name;
}

long answer(std::string_view request, std::uint32_t port, const std::function<std::vector<Interface>()> &interfaces,
            std::vector<std::string> &replies, const std::function<std::vector<HostRoute>()> &routes) {
    // SELinux looks at every message first, as far as their lengths are sound (selinux_nlmsg_perm).
    for (std::size_t at = 0; request.size() - at >= header_size;) {
        const std::uint32_t length = read32(request, at);
        if (length < header_size || length > request.size() - at) break;
        if (const long refused = permission(read16(request, at + 4))) return refused;
        at += std::min(aligned(length), request.size() - at);
    }
    // netlink_rcv_skb: each request in turn, junk ending the walk; the host's interfaces once for all of them.
    std::vector<Interface> known;
    bool asked = false;
    const std::function<std::vector<Interface>()> once = [&] {
        if (!asked) {
            known = interfaces();
            asked = true;
        }
        return known;
    };
    std::vector<HostRoute> known_routes;
    bool asked_routes = false;
    const std::function<std::vector<HostRoute>()> routes_once = [&] {
        if (!asked_routes && routes) known_routes = routes();
        asked_routes = true;
        return known_routes;
    };
    for (std::size_t at = 0; request.size() - at >= header_size;) {
        const std::uint32_t length = read32(request, at);
        if (length < header_size || length > request.size() - at) break;
        const std::string_view one = request.substr(at, length);
        const auto flags = read16(one, 6);
        long result = 0;
        if ((flags & nlm_f_request) != 0) result = respond(one, port, once, routes_once, replies);
        if (result != dumped && ((flags & nlm_f_ack) != 0 || result != 0)) {
            replies.push_back(acknowledgement(result, one, port));
        }
        at += std::min(aligned(length), request.size() - at);
    }
    return 0;
}

bool is_interface_ioctl(std::uint32_t request) {
    switch (request) {
        case siocgifname:
        case siocgifconf:
        case siocgifflags:
        case siocgifaddr:
        case siocgifdstaddr:
        case siocgifbrdaddr:
        case siocgifnetmask:
        case siocgifmtu:
        case siocgifhwaddr:
        case siocgifindex: return true;
        default: return false;
    }
}

long interface_ioctl(std::uint32_t request, std::uint64_t argument, bool inet, bool &handled) {
    handled = is_interface_ioctl(request) && request != siocgifhwaddr;
    if (!handled) return 0;
    if (request == siocgifconf) return interface_list(argument);
    // The guest's ifreq, read once and written back whole, as get_user_ifreq and put_user_ifreq copy it.
    LinuxIfreq ifr;
    if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&ifr), argument, sizeof ifr)) {
        return -linux_errno::efault;
    }
    long result = -linux_errno::enodev;
    switch (request) {
        case siocgifname: {
            for (const auto &each : interfaces()) {
                if (static_cast<std::int32_t>(each.index) != ifr.value) continue;
                std::strncpy(ifr.name, each.name.c_str(), sizeof ifr.name - 1);
                ifr.name[std::min<std::size_t>(each.name.size(), sizeof ifr.name - 1)] = '\0';
                result = 0;
                break;
            }
            break;
        }
        case siocgifindex:
        case siocgifflags:
        case siocgifmtu: {
            const auto all = interfaces();
            if (const Interface *interface = by_name(all, ifr.name)) {
                if (request == siocgifindex) ifr.value = static_cast<std::int32_t>(interface->index);
                if (request == siocgifflags) ifr.flags = static_cast<std::int16_t>(interface->flags);
                if (request == siocgifmtu) ifr.value = interface->mtu;
                result = 0;
            }
            break;
        }
        default:
            // SIOCGIFADDR, SIOCGIFDSTADDR, SIOCGIFBRDADDR and SIOCGIFNETMASK: AF_INET's inet_ioctl; on another socket
            // dev_ioctl has none of them.
            result = inet ? inet_getter(request, ifr) : -linux_errno::enotty;
            break;
    }
    if (result != 0) return result;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&ifr), sizeof ifr)) {
        return -linux_errno::efault;
    }
    return 0;
}

}  // namespace apkrt::netlink
