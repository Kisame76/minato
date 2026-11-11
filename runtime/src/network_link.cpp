#include "network_link.h"

#include "netlink.h"

#include <net/if.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cstring>

namespace apkrt::network::link {
namespace {

// netlink's Address/HostRoute carry Linux's family (2 IPv4, 10 IPv6); the wire form is 4/6. Linux interface flags:
// IFF_UP 0x1, IFF_LOOPBACK 0x8 (the guest-form Interface.flags are Linux's). Darwin's NET_RT_DUMP (not in the iOS SDK).
constexpr int linux_v4 = 2, iff_up = 0x1, iff_loopback = 0x8, net_rt_dump = 1;  // IPv6 is Linux family 10

// The Mac session's fake path monitor fixes the resolver too (fake_path_monitor.cpp): when set, read() returns these
// servers instead of the host's, so a run under MINATO_FAKE_NETWORK reads nothing of the host's network.
bool fake_dns_set = false;
std::vector<Address> fake_dns;

int wire_family(int linux_family) { return linux_family == linux_v4 ? 4 : 6; }

bool link_local(int linux_family, const std::string &bytes) {
    if (bytes.size() < 2) return false;
    const auto first = static_cast<unsigned char>(bytes[0]);
    const auto second = static_cast<unsigned char>(bytes[1]);
    if (linux_family == linux_v4) return first == 169 && second == 254;  // 169.254.0.0/16
    return first == 0xfe && (second & 0xc0) == 0x80;                     // fe80::/10
}

bool up_non_loopback(const netlink::Interface &interface) {
    return (interface.flags & iff_up) != 0 && (interface.flags & iff_loopback) == 0;
}

bool has_routable(const netlink::Interface &interface) {
    return std::any_of(interface.addresses.begin(), interface.addresses.end(),
                       [](const netlink::Address &a) { return !link_local(a.family, a.bytes); });
}

/// The network address of `bytes`/`prefix`: the host bits beyond the prefix cleared.
std::string masked(const std::string &bytes, int prefix) {
    std::string out = bytes;
    for (std::size_t i = 0; i < out.size(); i++) {
        const int bit = static_cast<int>(i) * 8;
        if (bit + 8 <= prefix) continue;                           // a whole byte inside the prefix
        if (bit >= prefix) { out[i] = 0; continue; }               // a whole byte outside it
        out[i] = static_cast<char>(static_cast<unsigned char>(out[i]) & (0xff << (8 - (prefix - bit))));
    }
    return out;
}

/// Whether sysctl NET_RT_DUMP can be read at all in this sandbox (a size query for IPv4).
bool route_dump_allowed() {
    int names[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, net_rt_dump, 0};
    std::size_t size = 0;
    return ::sysctl(names, 6, nullptr, &size, nullptr, 0) == 0;
}

void read_resolver(Snapshot &snapshot) {
    if (fake_dns_set) {  // the fake path monitor's servers; do not read the host's resolver
        snapshot.resolver_ok = true;
        snapshot.dns = fake_dns;
        return;
    }
    struct __res_state state;
    std::memset(&state, 0, sizeof state);
    if (::res_ninit(&state) != 0) return;  // resolver_ok stays false, dns empty
    snapshot.resolver_ok = true;
    union res_sockaddr_union servers[MAXNS];
    const int count = ::res_getservers(&state, servers, MAXNS);
    for (int i = 0; i < count && i < MAXNS; i++) {
        const int family = servers[i].sin.sin_family;
        if (family == AF_INET) {
            snapshot.dns.push_back({4, std::string(reinterpret_cast<const char *>(&servers[i].sin.sin_addr), 4), 0});
        } else if (family == AF_INET6) {
            snapshot.dns.push_back({6, std::string(reinterpret_cast<const char *>(&servers[i].sin6.sin6_addr), 16), 0});
        }
    }
    ::res_ndestroy(&state);
}

/// The active interface: the one a default route names (its index), else the first up non-loopback one with a routable
/// (non-link-local) address.
const netlink::Interface *active(const std::vector<netlink::Interface> &interfaces,
                                 const std::vector<netlink::HostRoute> &routes) {
    for (const auto &route : routes)
        for (const auto &interface : interfaces)
            if (interface.index == route.index && up_non_loopback(interface)) return &interface;
    for (const auto &interface : interfaces)
        if (up_non_loopback(interface) && has_routable(interface)) return &interface;
    return nullptr;
}

void put_u8(std::string &out, unsigned value) { out.push_back(static_cast<char>(value & 0xff)); }
void put_u32le(std::string &out, std::uint32_t value) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

}  // namespace

void set_fake_dns(std::vector<Address> servers) {
    fake_dns = std::move(servers);
    fake_dns_set = true;
}

void clear_fake_dns() {
    fake_dns_set = false;
    fake_dns.clear();
}

std::string serialize(const Snapshot &s) {
    const auto u = [](int v) { return static_cast<unsigned>(v); };
    const auto count = [](std::size_t n) { return static_cast<unsigned>(n); };
    std::string out;
    put_u8(out, 1u);  // version
    put_u8(out, (s.resolver_ok ? 1u : 0u) | (s.route_dump_ok ? 2u : 0u));
    put_u8(out, count(s.iface.size()));
    out.append(s.iface);
    put_u32le(out, static_cast<std::uint32_t>(s.mtu));
    put_u8(out, count(s.addresses.size()));
    for (const auto &a : s.addresses) {
        put_u8(out, u(a.family));
        put_u8(out, u(a.prefix));
        out.append(a.bytes);
    }
    put_u8(out, count(s.dns.size()));
    for (const auto &d : s.dns) {
        put_u8(out, u(d.family));
        out.append(d.bytes);
    }
    put_u8(out, count(s.routes.size()));
    for (const auto &r : s.routes) {
        put_u8(out, u(r.dest_family));
        put_u8(out, u(r.dest_prefix));
        out.append(r.dest);
        put_u8(out, r.gateway.empty() ? 0u : 1u);
        if (!r.gateway.empty()) out.append(r.gateway);
    }
    return out;
}

Snapshot read() {
    Snapshot snapshot;
    const std::vector<netlink::Interface> interfaces = netlink::interfaces();
    const std::vector<netlink::HostRoute> routes = netlink::host_default_routes();
    snapshot.route_dump_ok = route_dump_allowed();
    if (const netlink::Interface *a = active(interfaces, routes)) {
        snapshot.have = true;
        snapshot.iface = a->name;
        snapshot.mtu = a->mtu;
        for (const auto &addr : a->addresses)
            snapshot.addresses.push_back({wire_family(addr.family), addr.bytes, addr.prefix});
        if (snapshot.route_dump_ok) {
            for (const auto &route : routes) {  // the default route(s) for this interface, gateway as the host gives it
                if (route.index != a->index) continue;
                const std::size_t width = route.family == linux_v4 ? 4u : 16u;
                snapshot.routes.push_back(
                    {wire_family(route.family), 0, std::string(width, '\0'), route.gateway});
            }
            for (const auto &addr : a->addresses) {  // on-link prefix routes, no gateway (over the link)
                if (link_local(addr.family, addr.bytes)) continue;
                snapshot.routes.push_back(
                    {wire_family(addr.family), addr.prefix, masked(addr.bytes, addr.prefix), std::string()});
            }
        }
    }
    read_resolver(snapshot);
    return snapshot;
}

}  // namespace apkrt::network::link
