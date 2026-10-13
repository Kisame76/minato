// Checks the guest's network interfaces and the kernel's netlink answers (ADR-0054 step c): the names an app sees for
// the host's interfaces, the bytes of RTM_GETADDR's dump as Android 15's kernel writes them, and what SELinux refuses
// an app. syscalls_test checks the socket around them, fixtures/suite/ifaddrs-test what bionic makes of it against
// Android 15.
#include "netlink.h"

#include "check.h"

#include <arpa/inet.h>
#include <net/route.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace std::string_literals;
using apkrt::netlink::Address;
using apkrt::netlink::HostInterface;
using apkrt::netlink::HostRoute;
using apkrt::netlink::Interface;

constexpr int af_inet = 2, af_inet6 = 10, af_packet = 17;
constexpr long eacces = 13, einval = 22, eopnotsupp = 95, enetunreach = 101;
constexpr std::uint16_t nlm_f_request = 1, nlm_f_multi = 2, nlm_f_ack = 4, nlm_f_dump = 0x300;
constexpr std::uint16_t nlmsg_noop = 1, nlmsg_error = 2, nlmsg_done = 3;
constexpr std::uint16_t rtm_newlink = 16, rtm_getlink = 18, rtm_setlink = 19, rtm_newaddr = 20, rtm_deladdr = 21,
                        rtm_getaddr = 22, rtm_getroute = 26, rtm_getneigh = 30, rtm_getrule = 34,
                        rtm_getneightbl = 66, rtm_gettunnel = 122;

// Darwin's interface flags (net/if.h).
constexpr unsigned darwin_up = 0x1, darwin_broadcast = 0x2, darwin_loopback = 0x8, darwin_pointopoint = 0x10,
                   darwin_smart = 0x20, darwin_running = 0x40, darwin_simplex = 0x800, darwin_multicast = 0x8000;

std::string le16(std::uint16_t value) {
    return std::string(reinterpret_cast<const char *>(&value), 2);
}

std::string le32(std::uint32_t value) {
    return std::string(reinterpret_cast<const char *>(&value), 4);
}

/// An address's bytes from its text.
std::string ip(const char *text) {
    unsigned char bytes[16];
    if (inet_pton(AF_INET, text, bytes) == 1) return std::string(reinterpret_cast<char *>(bytes), 4);
    CHECK(inet_pton(AF_INET6, text, bytes) == 1);
    return std::string(reinterpret_cast<char *>(bytes), 16);
}

Address address(const char *text, int prefix, const char *broadcast = nullptr, const char *peer = nullptr) {
    Address result;
    result.bytes = ip(text);
    result.family = result.bytes.size() == 4 ? af_inet : af_inet6;
    result.prefix = prefix;
    if (broadcast != nullptr) result.broadcast = ip(broadcast);
    if (peer != nullptr) result.peer = ip(peer);
    return result;
}

/// A netlink attribute: its length, its type and its payload, padded to 4 bytes.
std::string attribute(std::uint16_t type, const std::string &payload) {
    std::string result = le16(static_cast<std::uint16_t>(4 + payload.size())) + le16(type) + payload;
    result.resize((result.size() + 3) & ~std::size_t{3}, '\0');
    return result;
}

/// A netlink message: nlmsghdr with its length, then `payload`.
std::string message(std::uint16_t type, std::uint16_t flags, std::uint32_t sequence, std::uint32_t port,
                    const std::string &payload) {
    return le32(static_cast<std::uint32_t>(16 + payload.size())) + le16(type) + le16(flags) + le32(sequence) +
           le32(port) + payload;
}

/// A request as bionic's NetlinkConnection sends it: nlmsghdr and rtgenmsg, padded to 20 bytes.
std::string request(std::uint16_t type, std::uint16_t flags, std::uint32_t sequence, int family = 0) {
    return message(type, flags, sequence, 0, std::string(1, static_cast<char>(family)) + std::string(3, '\0'));
}

/// Linux's ifaddrmsg.
std::string ifaddrmsg(int family, int prefix, int flags, int scope, std::uint32_t index) {
    return std::string{static_cast<char>(family), static_cast<char>(prefix), static_cast<char>(flags),
                       static_cast<char>(scope)} +
           le32(index);
}

constexpr int ifa_f_permanent = 0x80, rt_scope_universe = 0, rt_scope_link = 253, rt_scope_host = 254;
constexpr std::uint16_t ifa_address = 1, ifa_local = 2, ifa_label = 3, ifa_broadcast = 4, ifa_cacheinfo = 6,
                        ifa_flags = 8;

/// IFA_CACHEINFO of a permanent address: lifetimes without end.
std::string cacheinfo() {
    return attribute(ifa_cacheinfo, le32(0xffffffff) + le32(0xffffffff) + le32(0) + le32(0));
}

/// RTM_NEWADDR for an IPv4 address as inet_fill_ifaddr writes it: IFA_ADDRESS (the peer of a point-to-point
/// interface), IFA_LOCAL, IFA_BROADCAST, IFA_LABEL, IFA_FLAGS and IFA_CACHEINFO.
std::string ipv4_message(std::uint32_t sequence, std::uint32_t port, std::uint32_t index, const char *label,
                         const char *local, int prefix, int scope, const char *broadcast = nullptr,
                         const char *peer = nullptr) {
    std::string payload = ifaddrmsg(af_inet, prefix, ifa_f_permanent, scope, index) +
                          attribute(ifa_address, ip(peer != nullptr ? peer : local)) + attribute(ifa_local, ip(local));
    if (broadcast != nullptr) payload += attribute(ifa_broadcast, ip(broadcast));
    payload += attribute(ifa_label, std::string(label) + '\0') + attribute(ifa_flags, le32(ifa_f_permanent)) +
               cacheinfo();
    return message(rtm_newaddr, nlm_f_multi, sequence, port, payload);
}

/// RTM_NEWADDR for an IPv6 address as inet6_fill_ifaddr writes it: IFA_ADDRESS, IFA_CACHEINFO and IFA_FLAGS.
std::string ipv6_message(std::uint32_t sequence, std::uint32_t port, std::uint32_t index, const char *text, int prefix,
                         int scope) {
    return message(rtm_newaddr, nlm_f_multi, sequence, port,
                   ifaddrmsg(af_inet6, prefix, ifa_f_permanent, scope, index) + attribute(ifa_address, ip(text)) +
                       cacheinfo() + attribute(ifa_flags, le32(ifa_f_permanent)));
}

/// NLMSG_DONE as netlink_dump_done writes it: the error 0 as its payload.
std::string done(std::uint32_t sequence, std::uint32_t port) {
    return message(nlmsg_done, nlm_f_multi, sequence, port, le32(0));
}

/// NLMSG_ERROR for `error` on `original`: the whole request, or its header alone for an acknowledgement.
std::string error_message(int error, const std::string &original, std::uint32_t port) {
    std::uint32_t sequence;
    std::memcpy(&sequence, original.data() + 8, 4);
    const std::string echoed = error == 0 ? original.substr(0, 16) : original;
    return message(nlmsg_error, error == 0 ? 0x100 : 0, sequence, port,
                   le32(static_cast<std::uint32_t>(error)) + echoed);
}

// --- The names ---------------------------------------------------------------------------------------------------

HostInterface host(const char *name, unsigned index, unsigned flags, int mtu, std::vector<Address> addresses = {}) {
    HostInterface result;
    result.name = name;
    result.index = index;
    result.flags = flags;
    result.mtu = mtu;
    result.addresses = std::move(addresses);
    return result;
}

/// An iPhone's interfaces on Wi-Fi with mobile data and a VPN, plus a USB Ethernet adapter.
std::vector<HostInterface> iphone() {
    constexpr unsigned ethernet = darwin_up | darwin_broadcast | darwin_smart | darwin_running | darwin_simplex |
                                  darwin_multicast;
    constexpr unsigned cellular = darwin_up | darwin_pointopoint | darwin_running | darwin_multicast;
    return {
        host("lo0", 1, darwin_up | darwin_loopback | darwin_running | darwin_multicast, 16384,
             {address("127.0.0.1", 8), address("::1", 128), address("fe80::1", 64)}),
        host("gif0", 2, darwin_pointopoint | darwin_multicast, 1280),
        host("pdp_ip0", 4, cellular, 1450, {address("10.20.30.40", 8, nullptr, "10.20.30.40")}),
        host("pdp_ip1", 5, cellular, 1450),
        host("pdp_ip2", 6, cellular, 1450, {address("2001:db8:1::7", 64)}),
        host("en0", 11, ethernet, 1500,
             {address("192.168.1.20", 24, "192.168.1.255"), address("fe80::1c2b:3cff:fe4d:5e6f", 64),
              address("2001:db8::5", 64)}),
        host("utun0", 12, cellular, 1380, {address("fe80::aa:bb", 64)}),
        host("utun1", 13, cellular, 1400, {address("fe80::cc:dd", 64), address("10.8.0.2", 24, nullptr, "10.8.0.1")}),
        host("utun2", 14, cellular, 1280, {address("2001:db8:2::9", 64)}),
        host("en2", 15, ethernet, 1500, {address("169.254.3.4", 16, "169.254.255.255")}),
        host("en3", 16, ethernet, 1500, {address("192.168.2.2", 24, "192.168.2.255")}),
        host("awdl0", 17, ethernet, 1500, {address("fe80::ee:ff", 64)}),
        host("llw0", 18, ethernet, 1500, {address("fe80::ee:ff", 64)}),
        host("anpi0", 19, ethernet, 1500, {address("fe80::12", 64)}),
        host("bridge0", 20, ethernet, 1500, {address("192.168.3.1", 24)}),
    };
}

TEST(the_guest_sees_androids_interfaces_with_the_hosts_indexes) {
    const auto guest = apkrt::netlink::guest_interfaces(iphone());
    std::vector<std::string> names;
    std::vector<unsigned> indexes;
    for (const auto &each : guest) {
        names.push_back(each.name);
        indexes.push_back(each.index);
    }
    CHECK((names == std::vector<std::string>{"lo", "rmnet_data0", "rmnet_data2", "wlan0", "tun0", "tun1", "eth0"}));
    CHECK((indexes == std::vector<unsigned>{1, 4, 6, 11, 13, 14, 16}));
    CHECK(guest.size() == 7 && guest[3].host_name == "en0" && guest[4].host_name == "utun1" &&
          guest[5].host_name == "utun2" && guest[6].host_name == "en3");
}

TEST(lo_is_linuxs_loopback) {
    const auto guest = apkrt::netlink::guest_interfaces(iphone());
    CHECK(!guest.empty() && guest[0].name == "lo");
    if (guest.empty()) return;
    const Interface &lo = guest[0];
    CHECK(lo.index == 1 && lo.flags == 0x49 && lo.mtu == 65536);  // UP, LOOPBACK, RUNNING, no MULTICAST
    CHECK(lo.addresses.size() == 2);  // lo0's fe80::1 is Darwin's
    if (lo.addresses.size() != 2) return;
    CHECK(lo.addresses[0].family == af_inet && lo.addresses[0].bytes == ip("127.0.0.1") && lo.addresses[0].prefix == 8);
    CHECK(lo.addresses[1].family == af_inet6 && lo.addresses[1].bytes == ip("::1") && lo.addresses[1].prefix == 128);
}

TEST(the_other_interfaces_keep_their_addresses_with_linuxs_flags) {
    const auto guest = apkrt::netlink::guest_interfaces(iphone());
    CHECK(guest.size() == 7);
    if (guest.size() != 7) return;
    const Interface &wlan = guest[3];
    CHECK(wlan.flags == 0x1043 && wlan.mtu == 1500);  // UP, BROADCAST, RUNNING and Linux's MULTICAST
    CHECK(wlan.addresses.size() == 3 && wlan.addresses[0].broadcast == ip("192.168.1.255") &&
          wlan.addresses[1].bytes == ip("fe80::1c2b:3cff:fe4d:5e6f"));
    const Interface &cellular = guest[1];
    CHECK(cellular.flags == 0x1051 && cellular.mtu == 1450);  // UP, POINTOPOINT, RUNNING, MULTICAST
    CHECK(cellular.addresses.size() == 1 && cellular.addresses[0].peer == ip("10.20.30.40"));
    CHECK(guest[4].addresses.size() == 2 && guest[4].addresses[1].peer == ip("10.8.0.1"));
}

TEST(wlan0_is_there_without_addresses_and_rmnet_tun_and_eth_only_with_them) {
    auto hosts = iphone();
    for (auto &each : hosts) {
        if (each.name != "lo0") each.addresses.clear();
    }
    const auto guest = apkrt::netlink::guest_interfaces(hosts);
    CHECK(guest.size() == 2 && guest[0].name == "lo" && guest[1].name == "wlan0" && guest[1].addresses.empty());
}

TEST(host_interfaces_have_lo0_and_link_local_addresses_without_the_scope) {
    const auto hosts = apkrt::netlink::host_interfaces();
    bool loopback = false, link_local = false;
    for (const auto &each : hosts) {
        if (each.name != "lo0") continue;
        CHECK(each.index == 1 && each.mtu > 0);
        for (const auto &one : each.addresses) {
            if (one.bytes == ip("127.0.0.1") && one.prefix == 8) loopback = true;
            if (one.family == af_inet6 && one.bytes == ip("fe80::1") && one.prefix == 64) link_local = true;
        }
    }
    CHECK(loopback);
    CHECK(link_local);  // getifaddrs moves KAME's scope from bytes 2 and 3 into sin6_scope_id
}

// --- The answers -------------------------------------------------------------------------------------------------

/// lo and wlan0 of the iPhone above.
std::vector<Interface> two() {
    auto guest = apkrt::netlink::guest_interfaces(iphone());
    return {guest.at(0), guest.at(3)};
}

std::vector<std::string> answered(const std::string &datagram, long expected = 0, std::uint32_t port = 1234,
                                  std::vector<Interface> interfaces = two(), std::vector<HostRoute> routes = {}) {
    std::vector<std::string> replies;
    const long result = apkrt::netlink::answer(datagram, port, [&] { return interfaces; }, replies,
                                               [&] { return routes; });
    CHECK(result == expected);
    return replies;
}

TEST(an_address_dump_answers_ipv4_then_ipv6_then_done) {
    const auto replies = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 7));
    CHECK(replies.size() == 3);
    if (replies.size() != 3) return;
    CHECK(replies[0] == ipv4_message(7, 1234, 1, "lo", "127.0.0.1", 8, rt_scope_host) +
                            ipv4_message(7, 1234, 11, "wlan0", "192.168.1.20", 24, rt_scope_universe,
                                         "192.168.1.255"));
    CHECK(replies[1] == ipv6_message(7, 1234, 1, "::1", 128, rt_scope_host) +
                            ipv6_message(7, 1234, 11, "fe80::1c2b:3cff:fe4d:5e6f", 64, rt_scope_link) +
                            ipv6_message(7, 1234, 11, "2001:db8::5", 64, rt_scope_universe));
    CHECK(replies[2] == done(7, 1234));
    CHECK(replies[2].size() == 20);
}

TEST(an_address_dump_of_one_family_answers_it_alone) {
    const auto inet = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 8, af_inet));
    CHECK(inet.size() == 2 && inet[0].size() == 76 + 88 && inet[1] == done(8, 1234));
    const auto inet6 = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 9, af_inet6));
    CHECK(inet6.size() == 2 && inet6[0].size() == 3 * 72 && inet6[1] == done(9, 1234));
    // A family without addresses of its own falls back to all of them, as rtnetlink_rcv_msg does.
    CHECK(answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 10, af_packet)).size() == 3);
}

TEST(a_point_to_point_address_names_its_peer_first) {
    auto guest = apkrt::netlink::guest_interfaces(iphone());
    const auto replies = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 3, af_inet), 0, 99, {guest.at(4)});
    CHECK(replies.size() == 2 && replies[0] == ipv4_message(3, 99, 13, "tun0", "10.8.0.2", 24, rt_scope_universe,
                                                            nullptr, "10.8.0.1"));
}

TEST(a_long_dump_comes_in_datagrams_of_one_page) {
    Interface many;
    many.name = "wlan0";
    many.index = 11;
    for (int each = 0; each < 100; ++each) {
        many.addresses.push_back(address(("2001:db8::" + std::to_string(each + 1)).c_str(), 64));
    }
    const auto replies = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 5, af_inet6), 0, 1, {many});
    CHECK(replies.size() == 3 && replies.back() == done(5, 1));
    std::size_t messages = 0;
    for (std::size_t each = 0; each + 1 < replies.size(); ++each) {
        CHECK(replies[each].size() <= 3776 && replies[each].size() % 72 == 0);  // NLMSG_GOODSIZE on 4 KiB pages
        messages += replies[each].size() / 72;
    }
    CHECK(messages == 100);
    if (replies.size() == 3) CHECK(replies[0].substr(0, 72) == ipv6_message(5, 1, 11, "2001:db8::1", 64, 0));
}

TEST(selinux_refuses_an_app_the_link_dump_the_neighbours_and_every_change) {
    for (const std::uint16_t type : {rtm_getlink, rtm_getneigh, rtm_getneightbl, rtm_newlink, rtm_setlink, rtm_newaddr,
                                     rtm_deladdr}) {
        CHECK(answered(request(type, nlm_f_request | nlm_f_dump, 1), -eacces).empty());
        CHECK(answered(request(type, nlm_f_request | nlm_f_ack, 1), -eacces).empty());
    }
    // SELinux looks at every message before the kernel answers any.
    CHECK(answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 1) + request(rtm_getlink, nlm_f_request, 2),
                   -eacces).empty());
    // A type that SELinux knows no permission for.
    CHECK(answered(request(nlmsg_noop, nlm_f_request | nlm_f_ack, 1), -einval).empty());
    CHECK(answered(request(200, nlm_f_request, 1), -einval).empty());
}

TEST(other_reads_answer_empty_dumps_and_errors) {
    CHECK((answered(request(rtm_getrule, nlm_f_request | nlm_f_dump, 5)) == std::vector<std::string>{done(5, 1234)}));
    CHECK((answered(request(rtm_gettunnel, nlm_f_request | nlm_f_dump, 6)) ==
           std::vector<std::string>{done(6, 1234)}));
    // A route lookup (a named departure: no lookup in the guest's tables), and requests that only dump.
    const std::string lookup = request(rtm_getroute, nlm_f_request, 7);
    CHECK((answered(lookup) == std::vector<std::string>{error_message(-enetunreach, lookup, 1234)}));
    const std::string rule = request(rtm_getrule, nlm_f_request, 8);
    CHECK((answered(rule) == std::vector<std::string>{error_message(-eopnotsupp, rule, 1234)}));
    const std::string one_address = request(rtm_getaddr, nlm_f_request, 9);
    CHECK((answered(one_address) == std::vector<std::string>{error_message(-eopnotsupp, one_address, 1234)}));
}

TEST(acknowledgements_and_messages_the_kernel_skips) {
    // NLM_F_ACK on a dump: the dump alone.
    CHECK(answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump | nlm_f_ack, 1)).size() == 3);
    // A message that is no request: nothing, or the acknowledgement it asks for.
    CHECK(answered(request(rtm_getaddr, nlm_f_dump, 2)).empty());
    const std::string no_request = request(rtm_getaddr, nlm_f_dump | nlm_f_ack, 3);
    CHECK((answered(no_request) == std::vector<std::string>{error_message(0, no_request, 1234)}));
    // A request too short for rtgenmsg: nothing but the acknowledgement.
    CHECK(answered(message(rtm_getaddr, nlm_f_request | nlm_f_dump, 4, 0, "")).empty());
    const std::string bare = message(rtm_getroute, nlm_f_request | nlm_f_ack, 5, 0, "");
    CHECK((answered(bare) == std::vector<std::string>{error_message(0, bare, 1234)}));
}

TEST(junk_ends_the_datagram_quietly) {
    CHECK(answered(std::string(10, '\x01')).empty());
    std::string longer = request(rtm_getaddr, nlm_f_request | nlm_f_dump, 1);
    longer.replace(0, 4, le32(1000));
    CHECK(answered(longer).empty());
    std::string shorter = request(rtm_getaddr, nlm_f_request | nlm_f_dump, 1);
    shorter.replace(0, 4, le32(8));
    CHECK(answered(shorter).empty());
    // A good request before the junk is answered.
    CHECK(answered(request(rtm_getrule, nlm_f_request | nlm_f_dump, 2) + std::string(6, '\0')).size() == 1);
}

TEST(two_requests_in_one_datagram_are_answered_in_order) {
    const auto replies = answered(request(rtm_getaddr, nlm_f_request | nlm_f_dump, 1, af_inet6) +
                                  request(rtm_getrule, nlm_f_request | nlm_f_dump, 2));
    CHECK(replies.size() == 3 && replies[1] == done(1, 1234) && replies[2] == done(2, 1234));
}

// --- The routes (ADR-0054 step c3) ------------------------------------------------------------------------------

constexpr std::uint16_t rtm_newroute = 24;
constexpr std::uint16_t rta_dst = 1, rta_oif = 4, rta_gateway = 5, rta_priority = 6, rta_prefsrc = 7, rta_metrics = 8,
                        rta_cacheinfo = 12, rta_table = 15, rta_pref = 20, rtax_hoplimit = 10;
constexpr int rtn_unicast = 1, rtn_local = 2, rtn_broadcast = 3, rtn_multicast = 5;
constexpr int rtprot_kernel = 2, rtprot_static = 4, rtprot_ra = 9;

/// A route as the test writes it: what both families have, and IPv6's metric.
struct Want {
    std::uint32_t table;
    int type, protocol, scope;
    const char *destination;  // nullptr: a default route
    int prefix;
    std::uint32_t oif;
    const char *source = nullptr, *gateway = nullptr;
    long metric = -1;  // IPv6
    bool hoplimit = false;
};

/// RTM_NEWROUTE as fib_dump_info writes it for IPv4: RTA_TABLE, RTA_DST, RTA_PREFSRC, RTA_GATEWAY and RTA_OIF; and as
/// rt6_fill_node for IPv6: RTA_TABLE, RTA_DST, RTA_METRICS, RTA_PRIORITY, RTA_GATEWAY, RTA_OIF, RTA_CACHEINFO and
/// RTA_PREF. The header's table is RT_TABLE_COMPAT for a table beyond 255.
std::string route(int family, std::uint32_t sequence, const Want &want) {
    std::string payload{static_cast<char>(family), static_cast<char>(want.prefix), 0, 0,
                        static_cast<char>(want.table < 256 ? want.table : 252), static_cast<char>(want.protocol),
                        static_cast<char>(want.scope), static_cast<char>(want.type)};
    payload += le32(0);
    payload += attribute(rta_table, le32(want.table));
    if (want.prefix > 0) payload += attribute(rta_dst, ip(want.destination));
    if (family == af_inet) {
        if (want.source != nullptr) payload += attribute(rta_prefsrc, ip(want.source));
    } else {
        if (want.hoplimit) payload += attribute(rta_metrics, attribute(rtax_hoplimit, le32(64)));
        payload += attribute(rta_priority, le32(static_cast<std::uint32_t>(want.metric)));
    }
    if (want.gateway != nullptr) payload += attribute(rta_gateway, ip(want.gateway));
    payload += attribute(rta_oif, le32(want.oif));
    if (family == af_inet6) payload += attribute(rta_cacheinfo, std::string(32, '\0')) + attribute(rta_pref, "\0"s);
    return message(rtm_newroute, nlm_f_multi, sequence, 1234, payload);
}

std::string routes(int family, std::uint32_t sequence, const std::vector<Want> &wants) {
    std::string out;
    for (const auto &want : wants) out += route(family, sequence, want);
    return out;
}

constexpr int host_scope = 254, link_scope = 253, universe = 0;

TEST(an_ipv4_route_dump_lays_the_tables_out_as_android_15) {
    // lo and wlan0 (the host's en0, index 11, 192.168.1.20/24) with the host's default route through 192.168.1.1:
    // netd's tables of wlan0's local routes and network first, as the kernel's table hash orders them, then main and
    // local, each by destination.
    const std::vector<HostRoute> host{{af_inet, 11, ip("192.168.1.1")}, {af_inet6, 11, ip("fe80::1")}};
    const auto replies = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 3, af_inet), 0, 1234, two(), host);
    CHECK(replies.size() == 2);
    if (replies.size() != 2) return;
    CHECK(replies[0] == routes(af_inet, 3, {
        {1000000011, rtn_unicast, rtprot_static, link_scope, "192.168.1.0", 24, 11},
        {1000000011, rtn_unicast, rtprot_static, link_scope, "224.0.0.0", 24, 11},
        {1011, rtn_unicast, rtprot_static, universe, nullptr, 0, 11, nullptr, "192.168.1.1"},
        {1011, rtn_unicast, rtprot_static, link_scope, "192.168.1.0", 24, 11},
        {254, rtn_unicast, rtprot_kernel, link_scope, "192.168.1.0", 24, 11, "192.168.1.20"},
        {255, rtn_local, rtprot_kernel, host_scope, "127.0.0.0", 8, 1, "127.0.0.1"},
        {255, rtn_local, rtprot_kernel, host_scope, "127.0.0.1", 32, 1, "127.0.0.1"},
        {255, rtn_broadcast, rtprot_kernel, link_scope, "127.255.255.255", 32, 1, "127.0.0.1"},
        {255, rtn_local, rtprot_kernel, host_scope, "192.168.1.20", 32, 11, "192.168.1.20"},
        {255, rtn_broadcast, rtprot_kernel, link_scope, "192.168.1.255", 32, 11, "192.168.1.20"},
    }));
    CHECK(replies[1] == done(3, 1234));
}

TEST(an_ipv6_route_dump_lays_the_tables_out_as_android_15) {
    // The kernel's routes (metric 256) beside netd's (1024), the router's default route from its advertisement with
    // its hop limit, the default route last in its table, and the addresses' local routes with ff00::/8 in table local.
    const std::vector<HostRoute> host{{af_inet, 11, ip("192.168.1.1")}, {af_inet6, 11, ip("fe80::1")}};
    const auto replies = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 4, af_inet6), 0, 1234, two(), host);
    CHECK(replies.size() == 2);
    if (replies.size() != 2) return;
    CHECK(replies[0] == routes(af_inet6, 4, {
        {1000000011, rtn_unicast, rtprot_static, universe, "2001:db8::", 64, 11, nullptr, nullptr, 1024},
        {1000000011, rtn_unicast, rtprot_static, universe, "fe80::", 64, 11, nullptr, nullptr, 1024},
        {1011, rtn_unicast, rtprot_kernel, universe, "2001:db8::", 64, 11, nullptr, nullptr, 256},
        {1011, rtn_unicast, rtprot_static, universe, "2001:db8::", 64, 11, nullptr, nullptr, 1024},
        {1011, rtn_unicast, rtprot_kernel, universe, "fe80::", 64, 11, nullptr, nullptr, 256},
        {1011, rtn_unicast, rtprot_static, universe, "fe80::", 64, 11, nullptr, nullptr, 1024},
        {1011, rtn_unicast, rtprot_ra, universe, nullptr, 0, 11, nullptr, "fe80::1", 1024, true},
        {255, rtn_local, rtprot_kernel, universe, "::1", 128, 1, nullptr, nullptr, 0},
        {255, rtn_local, rtprot_kernel, universe, "2001:db8::5", 128, 11, nullptr, nullptr, 0},
        {255, rtn_local, rtprot_kernel, universe, "fe80::1c2b:3cff:fe4d:5e6f", 128, 11, nullptr, nullptr, 0},
        {255, rtn_multicast, rtprot_kernel, universe, "ff00::", 8, 11, nullptr, nullptr, 256},
    }));
    CHECK(replies[1] == done(4, 1234));
}

TEST(a_route_dump_of_both_families_answers_ipv4_then_ipv6_and_without_the_hosts_routes_no_default) {
    const auto replies = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 5));
    CHECK(replies.size() == 3);
    if (replies.size() != 3) return;
    CHECK(replies[0].find(ip("192.168.1.1")) == std::string::npos);
    CHECK(replies[0].find(ip("224.0.0.0")) != std::string::npos);
    CHECK(replies[1].substr(16, 1) == std::string(1, static_cast<char>(af_inet6)));
    CHECK(replies[2] == done(5, 1234));
}

TEST(a_point_to_point_address_routes_its_peer_and_an_ipv6_address_without_a_prefix_routes_nothing) {
    // A VPN's 10.2.0.2 to its peer 10.2.0.1, /32: a route to the peer in table main, as fib_add_ifaddr adds where the
    // prefix is not the address; no broadcast. An IPv6 address of prefix 0 gives no ::/0 to the network's table.
    auto guest = apkrt::netlink::guest_interfaces(iphone());
    Interface tun = guest.at(3);
    tun.addresses = {address("10.2.0.2", 32, nullptr, "10.2.0.1"), address("2001:db8:5::1", 0)};
    const auto four = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 8, af_inet), 0, 1234, {tun});
    CHECK(four.size() == 2);
    if (four.size() == 2) {
        CHECK(four[0] == routes(af_inet, 8, {
            {1000000011, rtn_unicast, rtprot_static, link_scope, "10.2.0.1", 32, 11},
            {1000000011, rtn_unicast, rtprot_static, link_scope, "224.0.0.0", 24, 11},
            {1011, rtn_unicast, rtprot_static, link_scope, "10.2.0.1", 32, 11},
            {254, rtn_unicast, rtprot_kernel, link_scope, "10.2.0.1", 32, 11, "10.2.0.2"},
            {255, rtn_local, rtprot_kernel, host_scope, "10.2.0.2", 32, 11, "10.2.0.2"},
        }));
    }
    const auto six = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 9, af_inet6), 0, 1234, {tun});
    CHECK(six.size() == 2);
    if (six.size() == 2) {
        CHECK(six[0] == routes(af_inet6, 9, {
            {255, rtn_local, rtprot_kernel, universe, "2001:db8:5::1", 128, 11, nullptr, nullptr, 0},
            {255, rtn_multicast, rtprot_kernel, universe, "ff00::", 8, 11, nullptr, nullptr, 256},
        }));
    }
}

TEST(a_route_dump_takes_each_prefix_once_and_none_of_a_zero_network) {
    // A second address in a subnet adds no route to it; a /31 has no broadcast; 0.0.0.0/8 is no prefix route
    // (fib_add_ifaddr's zeronet); a link-local address gives fe80::/64 whatever its own prefix.
    auto guest = apkrt::netlink::guest_interfaces(iphone());
    Interface wlan = guest.at(3);
    wlan.addresses = {address("192.168.1.20", 24, "192.168.1.255"), address("192.168.1.21", 24, "192.168.1.255"),
                      address("10.3.0.1", 31), address("0.1.2.3", 8), address("fe80::2", 96)};
    const auto four = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 10, af_inet), 0, 1234, {wlan});
    CHECK(four.size() == 2);
    if (four.size() == 2) {
        CHECK(four[0] == routes(af_inet, 10, {
            {1000000011, rtn_unicast, rtprot_static, link_scope, "10.3.0.0", 31, 11},
            {1000000011, rtn_unicast, rtprot_static, link_scope, "192.168.1.0", 24, 11},
            {1000000011, rtn_unicast, rtprot_static, link_scope, "224.0.0.0", 24, 11},
            {1011, rtn_unicast, rtprot_static, link_scope, "10.3.0.0", 31, 11},
            {1011, rtn_unicast, rtprot_static, link_scope, "192.168.1.0", 24, 11},
            {254, rtn_unicast, rtprot_kernel, link_scope, "10.3.0.0", 31, 11, "10.3.0.1"},
            {254, rtn_unicast, rtprot_kernel, link_scope, "192.168.1.0", 24, 11, "192.168.1.20"},
            {255, rtn_local, rtprot_kernel, host_scope, "0.1.2.3", 32, 11, "0.1.2.3"},
            {255, rtn_local, rtprot_kernel, host_scope, "10.3.0.1", 32, 11, "10.3.0.1"},
            {255, rtn_local, rtprot_kernel, host_scope, "192.168.1.20", 32, 11, "192.168.1.20"},
            {255, rtn_local, rtprot_kernel, host_scope, "192.168.1.21", 32, 11, "192.168.1.21"},
            {255, rtn_broadcast, rtprot_kernel, link_scope, "192.168.1.255", 32, 11, "192.168.1.20"},
        }));
    }
    const auto six = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 11, af_inet6), 0, 1234, {wlan});
    CHECK(six.size() == 2);
    if (six.size() == 2) {
        CHECK(six[0] == routes(af_inet6, 11, {
            {1000000011, rtn_unicast, rtprot_static, universe, "fe80::", 64, 11, nullptr, nullptr, 1024},
            {1011, rtn_unicast, rtprot_kernel, universe, "fe80::", 64, 11, nullptr, nullptr, 256},
            {1011, rtn_unicast, rtprot_static, universe, "fe80::", 64, 11, nullptr, nullptr, 1024},
            {255, rtn_local, rtprot_kernel, universe, "fe80::2", 128, 11, nullptr, nullptr, 0},
            {255, rtn_multicast, rtprot_kernel, universe, "ff00::", 8, 11, nullptr, nullptr, 256},
        }));
    }
}

TEST(a_long_route_dump_comes_in_datagrams_of_one_page) {
    auto guest = apkrt::netlink::guest_interfaces(iphone());
    Interface many = guest.at(3);
    for (int i = 0; i < 60; i++) {
        many.addresses.push_back(address(("10.1." + std::to_string(i) + ".1").c_str(), 24, nullptr));
    }
    const auto replies = answered(request(rtm_getroute, nlm_f_request | nlm_f_dump, 6, af_inet), 0, 1234,
                                  {guest.at(0), many});
    CHECK(replies.size() > 2);
    for (std::size_t i = 0; i + 1 < replies.size(); i++) CHECK(replies[i].size() <= 3776);
}

// --- The host's routes -------------------------------------------------------------------------------------------

/// A sockaddr as Darwin's routing messages carry it, padded to 4 bytes (or 4 bytes for an empty one).
std::string sockaddr_bytes(int family, const std::string &address, std::size_t length = 0) {
    std::string out;
    if (family == 0) {  // a netmask: its length, no family, then the port's and the address's bytes it keeps
        out = std::string(1, static_cast<char>(length)) + std::string(1, '\0') + address;
        out.resize(length);
    } else if (family == AF_INET) {
        out = std::string{16, AF_INET} + std::string(2, '\0') + address + std::string(8, '\0');
    } else if (family == AF_INET6) {
        out = std::string{28, AF_INET6} + std::string(6, '\0') + address + std::string(4, '\0');
    } else {  // AF_LINK, its index
        out = std::string{20, 18} + address + std::string(16, '\0');
    }
    out.resize(out.empty() ? 4 : (out.size() + 3) & ~std::size_t{3}, '\0');
    return out;
}

/// rt_msghdr (net/route.h, 92 bytes) for a route of `index` with `flags`, and its sockaddrs, the first three of
/// RTAX_DST, RTAX_GATEWAY and RTAX_NETMASK in order.
std::string darwin_route(unsigned index, int flags, const std::vector<std::string> &addresses, int version = 5) {
    std::string body;
    int mask = 0;
    for (std::size_t i = 0; i < addresses.size(); i++) {
        body += addresses[i];
        mask |= 1 << i;
    }
    std::string header(92, '\0');
    const auto length = static_cast<std::uint16_t>(92 + body.size());
    std::memcpy(header.data(), &length, 2);
    header[2] = static_cast<char>(version);
    header[3] = 4;  // RTM_GET
    const auto short_index = static_cast<std::uint16_t>(index);
    std::memcpy(header.data() + 4, &short_index, 2);
    std::memcpy(header.data() + 8, &flags, 4);
    std::memcpy(header.data() + 12, &mask, 4);
    return header + body;
}

constexpr int rtf_up = 0x1, rtf_gateway = 0x2, rtf_host = 0x4, rtf_reject = 0x8, rtf_static = 0x800,
              rtf_llinfo = 0x400, rtf_blackhole = 0x1000, rtf_wascloned = 0x20000, rtf_ifscope = 0x1000000;

TEST(the_hosts_default_routes_come_from_its_routing_messages) {
    const std::string any4(4, '\0'), any6(16, '\0');
    std::string dump;
    // en0's default route, scoped and not, through 192.168.1.1: one route.
    dump += darwin_route(11, rtf_up | rtf_gateway | rtf_static, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("192.168.1.1")), sockaddr_bytes(0, "", 0)});
    dump += darwin_route(11, rtf_up | rtf_gateway | rtf_static | rtf_ifscope, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("192.168.1.1")), sockaddr_bytes(0, "", 0)});
    // A subnet, a host route, an ARP entry, a cloned route and one that is down: none is a default route.
    dump += darwin_route(11, rtf_up, {sockaddr_bytes(AF_INET, ip("192.168.1.0")),
                         sockaddr_bytes(18, std::string("\x0b\0", 2)),
                         sockaddr_bytes(0, std::string("\0\0\xff\xff\xff", 5), 7)});
    dump += darwin_route(11, rtf_up | rtf_gateway | rtf_host, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("192.168.1.9"))});
    dump += darwin_route(11, rtf_up | rtf_llinfo,
                         {sockaddr_bytes(AF_INET, any4), sockaddr_bytes(18, std::string(2, '\0'))});
    dump += darwin_route(12, rtf_up | rtf_gateway | rtf_wascloned, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("10.0.0.1"))});
    dump += darwin_route(13, rtf_gateway, {sockaddr_bytes(AF_INET, any4), sockaddr_bytes(AF_INET, ip("10.0.0.2"))});
    // A route to one address through a gateway without a netmask, and a VPN's reject and blackhole defaults.
    dump += darwin_route(12, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET, ip("10.0.0.0")),
                         sockaddr_bytes(AF_INET, ip("10.0.0.1"))});
    dump += darwin_route(12, rtf_up | rtf_gateway | rtf_reject, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("10.0.0.1"))});
    dump += darwin_route(12, rtf_up | rtf_gateway | rtf_blackhole, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("10.0.0.1"))});
    // A VPN's half of the address space, 0.0.0.0/1, and a gateway that is no address of the family.
    dump += darwin_route(12, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("10.0.0.1")), sockaddr_bytes(0, std::string("\0\0\x80", 3), 5)});
    dump += darwin_route(12, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(18, std::string("\x0c\0", 2))});
    // A wrong version, then pdp_ip0's default route over its link, without a gateway.
    dump += darwin_route(4, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET, any4),
                         sockaddr_bytes(AF_INET, ip("10.9.9.9"))}, 4);
    dump += darwin_route(4, rtf_up | rtf_static,
                         {sockaddr_bytes(AF_INET, any4), sockaddr_bytes(18, std::string("\x04\0", 2))});
    // en0's IPv6 router, whose link-local address carries KAME's scope in bytes 2 and 3.
    dump += darwin_route(11, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET6, any6),
                         sockaddr_bytes(AF_INET6, ip("fe80:b::1")), sockaddr_bytes(0, "", 0)});
    const auto found = apkrt::netlink::default_routes(dump);
    CHECK(found.size() == 3);
    if (found.size() != 3) return;
    CHECK(found[0].family == af_inet && found[0].index == 11 && found[0].gateway == ip("192.168.1.1"));
    CHECK(found[1].family == af_inet && found[1].index == 4 && found[1].gateway.empty());
    CHECK(found[2].family == af_inet6 && found[2].index == 11 && found[2].gateway == ip("fe80::1"));
}

TEST(a_routing_message_that_does_not_fit_ends_the_host_routes) {
    const std::string any4(4, '\0');
    const std::string good = darwin_route(11, rtf_up | rtf_gateway, {sockaddr_bytes(AF_INET, any4),
                                          sockaddr_bytes(AF_INET, ip("192.168.1.1"))});
    // A sockaddr longer than its message is skipped with the message; a message longer than the dump, or shorter
    // than its header, ends it.
    std::string overlong = good;
    overlong[92 + 16] = 120;  // the gateway's sa_len
    std::string other = good;
    other[4] = 12;  // its interface
    const auto kept = apkrt::netlink::default_routes(overlong + other);
    CHECK(kept.size() == 1 && kept[0].index == 12);
    std::string truncated = good;
    truncated.resize(good.size() - 4);
    CHECK(apkrt::netlink::default_routes(truncated).empty());
    std::string short_header = good;
    short_header[0] = 40, short_header[1] = 0;
    CHECK(apkrt::netlink::default_routes(short_header + good).empty());
    CHECK(apkrt::netlink::default_routes(std::string(3, '\x7f')).empty());
    // A gateway too short for its family is no route.
    std::string short_gateway = good;
    short_gateway[92 + 16] = 6;
    CHECK(apkrt::netlink::default_routes(short_gateway).empty());
}

TEST(the_hosts_routing_message_header_is_darwins) {
    CHECK(sizeof(rt_msghdr) == 92 && offsetof(rt_msghdr, rtm_index) == 4 && offsetof(rt_msghdr, rtm_flags) == 8 &&
          offsetof(rt_msghdr, rtm_addrs) == 12);
    CHECK(RTF_GATEWAY == rtf_gateway && RTF_HOST == rtf_host && RTF_LLINFO == rtf_llinfo &&
          RTF_WASCLONED == rtf_wascloned && RTF_IFSCOPE == rtf_ifscope && RTF_REJECT == rtf_reject &&
          RTF_BLACKHOLE == rtf_blackhole && RTM_VERSION == 5);
}

TEST(the_kernels_name_is_port_0_without_groups) {
    CHECK(apkrt::netlink::kernel_name() == le16(16) + le16(0) + le32(0) + le32(0));
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
