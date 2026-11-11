// The LinkProperties snapshot the runtime hands the framework (ADR-0054 step d): an app's getLinkProperties needs the
// active network's interface name, its addresses with prefix lengths and scope, the DNS servers, the default and
// on-link routes and the MTU. The snapshot is built from ONE source -- apkrt::netlink::interfaces() and
// host_default_routes() (the same functions the guest's getifaddrs and RTM_GETADDR/RTM_GETROUTE answer from) and the
// host resolver (res_getservers) -- so LinkProperties equals what the app's own getifaddrs/netlink see, no second
// source and no framework guess. Taken with each path update, handed to LocalNetwork.link via APKRT_NETWORK_LINK.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apkrt::network::link {

/// An address or a DNS server: wire family (4 or 6), the 4 or 16 bytes, and a prefix length (0 for a DNS server).
struct Address {
    int family = 0;  // 4 or 6 (the serialized form, not Linux's 2/10)
    std::string bytes;
    int prefix = 0;
};

/// A route: the destination's family, prefix and bytes, and a gateway (empty for an on-link route over the link). A
/// default route has prefix 0 and all-zero destination bytes; the gateway is never guessed (empty when there is none).
struct Route {
    int dest_family = 0;  // 4 or 6
    int dest_prefix = 0;
    std::string dest;
    std::string gateway;  // empty => on-link (no gateway)
};

/// What the active network's LinkProperties holds. Counts and flags only name whether a field could be read; no value
/// is invented. `have` is false when there is no active interface.
struct Snapshot {
    bool have = false;
    std::string iface;  // the guest name: wlan0, rmnet_data0, eth0
    std::int32_t mtu = 0;
    std::vector<Address> addresses;
    std::vector<Address> dns;
    std::vector<Route> routes;
    bool resolver_ok = false;    // res_ninit could read the resolver config
    bool route_dump_ok = false;  // sysctl NET_RT_DUMP was allowed (else routes is empty, never a guessed gateway)
};

/// The serialized snapshot LocalNetwork.link parses. Version 1, little-endian; every count fits a byte (one interface):
///   u8 version=1, u8 flags(bit0 resolver, bit1 routeDump), u8 ifaceLen, iface, u32 mtu,
///   u8 addrCount{u8 family(4|6), u8 prefix, 4|16 bytes}, u8 dnsCount{u8 family, 4|16 bytes},
///   u8 routeCount{u8 destFamily, u8 destPrefix, 4|16 destBytes, u8 hasGateway, [4|16 gatewayBytes]}
std::string serialize(const Snapshot &snapshot);

/// Reads the host now from the one source (netlink::interfaces() + host_default_routes() + res_getservers +
/// SIOCGIFMTU). Device-dependent (the sandbox decides routes/DNS), so not unit-tested; serialize() is.
Snapshot read();

/// The Mac session's fake path monitor fixes the resolver too (fake_path_monitor.cpp): set_fake_dns makes read()
/// return these servers instead of the host's, so a run under the fake reads nothing of the host's network;
/// clear_fake_dns restores res_getservers. Set once before the guest runs; tests set and clear it.
void set_fake_dns(std::vector<Address> servers);
void clear_fake_dns();

}  // namespace apkrt::network::link
