#include "network_probe.h"

#include "APKRuntime/apkrt.h"
#include "netlink.h"

#include <net/if.h>
#include <resolv.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace apkrt::network::probe {
namespace {

// Address.family carries Linux's family (netlink.h): 2 for IPv4, 10 for IPv6.
constexpr int family_v4 = 2, family_v6 = 10;
// Darwin's NET_RT_DUMP (net/route.h is not in the iOS SDK; netlink.cpp uses the same value).
constexpr int net_rt_dump = 1;

bool link_local(const netlink::Address &address) {
    if (address.bytes.size() < 2) return false;
    const auto first = static_cast<unsigned char>(address.bytes[0]);
    const auto second = static_cast<unsigned char>(address.bytes[1]);
    if (address.family == family_v4) return first == 169 && second == 254;       // 169.254.0.0/16
    return first == 0xfe && (second & 0xc0) == 0x80;                             // fe80::/10
}

bool private_v4(const netlink::Address &address) {
    if (address.family != family_v4 || address.bytes.size() < 2) return false;
    const auto first = static_cast<unsigned char>(address.bytes[0]);
    const auto second = static_cast<unsigned char>(address.bytes[1]);
    return first == 10                                       // 10.0.0.0/8
           || (first == 172 && (second & 0xf0) == 16)        // 172.16.0.0/12
           || (first == 192 && second == 168);               // 192.168.0.0/16
}

bool prefix_in_range(const netlink::Address &address) {
    const int max = address.family == family_v4 ? 32 : 128;
    return address.prefix >= 1 && address.prefix <= max;
}

bool has_routable_address(const netlink::HostInterface &interface) {
    for (const auto &address : interface.addresses)
        if (!link_local(address)) return true;
    return false;
}

bool up_non_loopback(const netlink::HostInterface &interface) {
    return (interface.flags & IFF_UP) != 0 && (interface.flags & IFF_LOOPBACK) == 0;
}

/// Whether sysctl NET_RT_DUMP (the routing table) can be read at all in this sandbox: a size query for IPv4.
bool route_dump_allowed() {
    int names[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, net_rt_dump, 0};
    std::size_t size = 0;
    return ::sysctl(names, 6, nullptr, &size, nullptr, 0) == 0;
}

void read_resolver(Reading &reading) {
    struct __res_state state;
    std::memset(&state, 0, sizeof state);
    if (::res_ninit(&state) != 0) return;  // resolver_ok stays false
    reading.resolver_ok = true;
    union res_sockaddr_union servers[MAXNS];
    const int count = ::res_getservers(&state, servers, MAXNS);
    for (int i = 0; i < count && i < MAXNS; i++) {
        const int family = servers[i].sin.sin_family;  // the family is at the same offset for sin and sin6
        if (family == AF_INET) {
            reading.dns++;
            reading.dns_v4++;
        } else if (family == AF_INET6) {
            reading.dns++;
            reading.dns_v6++;
        }
    }
    ::res_ndestroy(&state);
}

/// The active interface: the one a default route names (its index), else the first routable up non-loopback one.
const netlink::HostInterface *active_interface(const std::vector<netlink::HostInterface> &interfaces,
                                               const std::vector<netlink::HostRoute> &routes) {
    for (const auto &route : routes)
        for (const auto &interface : interfaces)
            if (interface.index == route.index && up_non_loopback(interface)) return &interface;
    for (const auto &interface : interfaces)
        if (up_non_loopback(interface) && has_routable_address(interface)) return &interface;
    return nullptr;
}

}  // namespace

Reading read() {
    Reading reading;
    const std::vector<netlink::HostInterface> interfaces = netlink::host_interfaces();
    for (const auto &interface : interfaces) {
        if (!up_non_loopback(interface)) continue;
        reading.interfaces++;
        if (has_routable_address(interface)) reading.routable++;
    }
    reading.route_dump_ok = route_dump_allowed();
    const std::vector<netlink::HostRoute> routes = netlink::host_default_routes();
    reading.default_route = !routes.empty();
    if (const netlink::HostInterface *active = active_interface(interfaces, routes)) {
        reading.active = true;
        reading.mtu = active->mtu;
        reading.prefix_ok = true;
        for (const auto &address : active->addresses) {
            if (!prefix_in_range(address)) reading.prefix_ok = false;
            if (address.family == family_v4) {
                reading.v4++;
                if (private_v4(address)) reading.private_v4 = true;
            } else if (address.family == family_v6) {
                reading.v6++;
                if (link_local(address)) reading.v6_link_local++;
                else reading.v6_global++;
            }
        }
    }
    read_resolver(reading);
    return reading;
}

std::string format(const Reading &r) {
    const auto yn = [](bool value) { return value ? "yes" : "no"; };
    char line[256];
    std::snprintf(line, sizeof line,
                  "net-probe interfaces=%d routable=%d active=%s v4=%d v6=%d v6global=%d v6ll=%d prefixok=%s "
                  "privatev4=%s mtu=%d dns=%d dnsv4=%d dnsv6=%d resolver=%s routedump=%s defaultroute=%s",
                  r.interfaces, r.routable, yn(r.active), r.v4, r.v6, r.v6_global, r.v6_link_local, yn(r.prefix_ok),
                  yn(r.private_v4), r.mtu, r.dns, r.dns_v4, r.dns_v6, r.resolver_ok ? "ok" : "fail",
                  r.route_dump_ok ? "ok" : "denied", yn(r.default_route));
    return line;
}

}  // namespace apkrt::network::probe

extern "C" int apkrt_network_probe(void) {
    const std::string line = "I/apkrt   : " + apkrt::network::probe::format(apkrt::network::probe::read()) + "\n";
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
    return 0;
}
