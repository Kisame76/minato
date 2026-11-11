// The Mac session's fake path monitor (ADR-0054 step d): the Mac (java_app_run) has no NWPathMonitor, so a guest app's
// getActiveNetwork is null and it sees no host network. Under MINATO_FAKE_NETWORK, session start calls
// start_fake_path_monitor(): it points netlink's host source at one fixed interface (wlan0, from en0) with a private
// IPv4 and a global and a link-local IPv6, a default route per family and the MTU, and reports a satisfied Wi-Fi path
// with apkrt_network_set. So the guest's getifaddrs/netlink and the LinkProperties snapshot read the same fixed values
// -- enough for net-link-test to run end to end on the Mac. The fixed values are documentation/private ranges only.
#include "fake_path_monitor.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "netlink.h"
#include "network_link.h"

#include <net/if.h>

#include <string>
#include <vector>

namespace apkrt::network {
namespace {

// netlink::Address carries Linux's family (2 IPv4, 10 IPv6).
constexpr int linux_inet = 2, linux_inet6 = 10;

std::string v4(unsigned a, unsigned b, unsigned c, unsigned d) {
    const unsigned char bytes[] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                                   static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
    return std::string(reinterpret_cast<const char *>(bytes), sizeof bytes);
}

/// A 16-byte IPv6 address: the first `head` bytes, zeros, then `last`.
std::string v6(std::vector<unsigned char> head, unsigned last) {
    std::string bytes(static_cast<std::size_t>(16), '\0');
    for (std::size_t i = 0; i < head.size() && i < 16; i++) bytes[i] = static_cast<char>(head[i]);
    bytes.back() = static_cast<char>(last);
    return bytes;
}

}  // namespace

void start_fake_path_monitor() {
    netlink::HostInterface en0;
    en0.name = "en0";  // guest_interfaces() names the first en0 wlan0
    en0.index = 2;     // lo0 is index 1
    en0.flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;  // Darwin flags; mapped to Linux's for the guest
    en0.mtu = 1500;
    netlink::Address ipv4;
    ipv4.family = linux_inet;
    ipv4.bytes = v4(192, 168, 7, 9);  // 192.168.7.9/24, a private LAN address
    ipv4.prefix = 24;
    ipv4.broadcast = v4(192, 168, 7, 255);
    netlink::Address global6;
    global6.family = linux_inet6;
    global6.bytes = v6({0x20, 0x01, 0x0d, 0xb8}, 9);  // 2001:db8::9/64, a documentation address
    global6.prefix = 64;
    netlink::Address link6;
    link6.family = linux_inet6;
    link6.bytes = v6({0xfe, 0x80}, 9);  // fe80::9/64 link-local (scope not in the bytes, as getifaddrs gives)
    link6.prefix = 64;
    en0.addresses = {ipv4, global6, link6};

    netlink::HostRoute ipv4_default{linux_inet, en0.index, v4(192, 168, 7, 1)};        // via the gateway
    netlink::HostRoute ipv6_default{linux_inet6, en0.index, v6({0xfe, 0x80}, 1)};       // via a link-local gateway
    netlink::set_fake_host({en0}, {ipv4_default, ipv6_default});
    // Fix the resolver too, so a run under the fake reads nothing of the host's network (private/doc addresses only).
    link::set_fake_dns({{4, v4(192, 168, 7, 1), 0}, {6, v6({0x20, 0x01, 0x0d, 0xb8}, 1), 0}});

    apkrt_network_set(APKRT_NETWORK_SATISFIED | APKRT_NETWORK_WIFI);
}

}  // namespace apkrt::network
