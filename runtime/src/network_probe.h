// Host-side network probe (a Minato diagnostic, not an Android feature): reads whether the iOS sandbox lets the runtime
// see what an app's LinkProperties needs -- the active interface's addresses and prefix lengths (getifaddrs), the
// resolver's servers (res_ninit/res_getservers), the default route (sysctl NET_RT_DUMP) and the MTU (SIOCGIFMTU). It
// writes ONE line of counts and families only to the session log (never an address, an interface name or an SSID),
// because it runs on the user's phone and network. The reading decides what the runtime-side snapshot can fill.
#pragma once

#include <string>

namespace apkrt::network::probe {

/// What the probe found, as counts and flags only -- there is no address, name or SSID field here, so a formatted
/// line can carry none.
struct Reading {
    int interfaces = 0;    // up, not loopback
    int routable = 0;      // up, not loopback, with an address that is not link-local
    bool active = false;   // an active interface was identified: the default route's, else the first routable one
    int v4 = 0, v6 = 0;    // the active interface's address count by family
    int v6_global = 0, v6_link_local = 0;  // its IPv6 addresses by scope
    bool prefix_ok = false;   // every counted address has a prefix length in 1..32 (v4) or 1..128 (v6)
    bool private_v4 = false;  // the active interface has an RFC1918 IPv4 address (a LAN address)
    int mtu = 0;              // the active interface's MTU
    int dns = 0, dns_v4 = 0, dns_v6 = 0;  // the resolver's configured servers, by family
    bool resolver_ok = false;    // res_ninit succeeded (the resolver config could be read at all)
    bool route_dump_ok = false;  // the sysctl NET_RT_DUMP was allowed (the sandbox did not refuse the routing table)
    bool default_route = false;  // a default route exists (IPv4 or IPv6)
};

/// The one line for `reading`: "net-probe key=value ...", counts and yes/no/ok values only. Never an address, name or
/// SSID -- the Reading holds none, so this cannot emit one.
std::string format(const Reading &reading);

/// Reads the host now: host_interfaces() (getifaddrs), host_default_routes() (sysctl NET_RT_DUMP), res_ninit/
/// res_getservers and SIOCGIFMTU. Device-dependent (the sandbox decides what succeeds), so not unit-tested.
Reading read();

}  // namespace apkrt::network::probe
