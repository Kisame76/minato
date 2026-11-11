// The LinkProperties snapshot's wire form (network_link.cpp serialize): what LocalNetwork.link parses. These build the
// fake path monitor's fixed values (wlan0 192.168.7.9/24, gateway 192.168.7.1, DNS 192.168.7.1, with a v6 pair) and a
// case for each refusal (no DNS servers, no routes), serialize, and parse back -- the same round trip the Java side
// does -- asserting the fields survive and the version/flags/mtu header is exactly as agreed with Formate.
#include "network_link.h"

#include "fake_path_monitor.h"
#include "netlink.h"

#include "check.h"

#include <cstdint>
#include <string>
#include <vector>

using apkrt::network::link::Address;
using apkrt::network::link::Route;
using apkrt::network::link::serialize;
using apkrt::network::link::Snapshot;

namespace {
std::string v4(int a, int b, int c, int d) {
    const unsigned char bytes[] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                                   static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
    return std::string(reinterpret_cast<const char *>(bytes), sizeof bytes);
}
// A 16-byte IPv6 address from its first bytes; the rest are zero, then the last byte is `last`.
std::string v6(std::initializer_list<int> head, int last) {
    std::string bytes(std::size_t{16}, '\0');
    std::size_t i = 0;
    for (int b : head) bytes[i++] = static_cast<char>(b);
    bytes.back() = static_cast<char>(last);
    return bytes;
}

// A minimal parser: the exact inverse of serialize(), as LocalNetwork.link's parse will be. Fills `out`, returns false
// on a malformed blob.
bool parse(const std::string &in, Snapshot &out) {
    std::size_t at = 0;
    auto u8 = [&](unsigned &v) {
        if (at >= in.size()) return false;
        v = static_cast<unsigned char>(in[at++]);
        return true;
    };
    auto bytes = [&](unsigned family, std::string &b) {
        const std::size_t n = family == 4u ? 4u : 16u;
        if (at + n > in.size()) return false;
        b = in.substr(at, n); at += n; return true;
    };
    unsigned version = 0, flags = 0, len = 0;
    if (!u8(version) || version != 1 || !u8(flags) || !u8(len)) return false;
    out.resolver_ok = (flags & 1) != 0;
    out.route_dump_ok = (flags & 2) != 0;
    if (at + len > in.size()) return false;
    out.iface = in.substr(at, len); at += len;
    if (at + 4 > in.size()) return false;
    const unsigned m = static_cast<unsigned>(static_cast<std::uint8_t>(in[at]))
                       | static_cast<unsigned>(static_cast<std::uint8_t>(in[at + 1])) << 8
                       | static_cast<unsigned>(static_cast<std::uint8_t>(in[at + 2])) << 16
                       | static_cast<unsigned>(static_cast<std::uint8_t>(in[at + 3])) << 24;
    out.mtu = static_cast<std::int32_t>(m);
    at += 4;
    unsigned count = 0;
    if (!u8(count)) return false;
    for (unsigned i = 0; i < count; i++) {
        unsigned fam = 0, prefix = 0; std::string b;
        if (!u8(fam) || !u8(prefix) || !bytes(fam, b)) return false;
        out.addresses.push_back({static_cast<int>(fam), b, static_cast<int>(prefix)});
    }
    if (!u8(count)) return false;
    for (unsigned i = 0; i < count; i++) {
        unsigned fam = 0; std::string b;
        if (!u8(fam) || !bytes(fam, b)) return false;
        out.dns.push_back({static_cast<int>(fam), b, 0});
    }
    if (!u8(count)) return false;
    for (unsigned i = 0; i < count; i++) {
        unsigned fam = 0, prefix = 0, has_gw = 0; std::string dest, gw;
        if (!u8(fam) || !u8(prefix) || !bytes(fam, dest) || !u8(has_gw)) return false;
        if (has_gw && !bytes(fam, gw)) return false;
        out.routes.push_back({static_cast<int>(fam), static_cast<int>(prefix), dest, gw});
    }
    return at == in.size();
}

bool same(const Snapshot &a, const Snapshot &b) {
    if (a.iface != b.iface || a.mtu != b.mtu || a.resolver_ok != b.resolver_ok || a.route_dump_ok != b.route_dump_ok)
        return false;
    if (a.addresses.size() != b.addresses.size() || a.dns.size() != b.dns.size() || a.routes.size() != b.routes.size())
        return false;
    for (std::size_t i = 0; i < a.addresses.size(); i++)
        if (a.addresses[i].family != b.addresses[i].family || a.addresses[i].prefix != b.addresses[i].prefix
            || a.addresses[i].bytes != b.addresses[i].bytes) return false;
    for (std::size_t i = 0; i < a.dns.size(); i++)
        if (a.dns[i].family != b.dns[i].family || a.dns[i].bytes != b.dns[i].bytes) return false;
    for (std::size_t i = 0; i < a.routes.size(); i++)
        if (a.routes[i].dest_family != b.routes[i].dest_family || a.routes[i].dest_prefix != b.routes[i].dest_prefix
            || a.routes[i].dest != b.routes[i].dest || a.routes[i].gateway != b.routes[i].gateway) return false;
    return true;
}

Snapshot wifi_fixed() {
    Snapshot s;
    s.have = true;
    s.iface = "wlan0";
    s.mtu = 1500;
    s.resolver_ok = true;
    s.route_dump_ok = true;
    s.addresses = {{4, v4(192, 168, 7, 9), 24}, {6, v6({0x20, 0x01, 0x0d, 0xb8}, 9), 64}};
    s.dns = {{4, v4(192, 168, 7, 1), 0}, {6, v6({0x20, 0x01, 0x0d, 0xb8}, 1), 0}};
    s.routes = {{4, 0, v4(0, 0, 0, 0), v4(192, 168, 7, 1)},                 // default v4 via the gateway
                {6, 0, v6({}, 0), v6({0xfe, 0x80}, 1)},                     // default v6 via a link-local gateway
                {4, 24, v4(192, 168, 7, 0), std::string()},                 // on-link v4, no gateway
                {6, 64, v6({0x20, 0x01, 0x0d, 0xb8}, 0), std::string()}};   // on-link v6, no gateway
    return s;
}
}  // namespace

TEST(the_wifi_fixed_values_round_trip_with_the_agreed_header) {
    const Snapshot in = wifi_fixed();
    const std::string blob = serialize(in);
    CHECK(static_cast<unsigned char>(blob[0]) == 1);  // version
    CHECK(static_cast<unsigned char>(blob[1]) == 3);  // flags: resolver + route dump
    CHECK(static_cast<unsigned char>(blob[2]) == 5);  // ifaceLen "wlan0"
    CHECK(blob.substr(3, 5) == "wlan0");
    CHECK(static_cast<unsigned char>(blob[8]) == 0xDC && static_cast<unsigned char>(blob[9]) == 0x05
          && static_cast<unsigned char>(blob[10]) == 0 && static_cast<unsigned char>(blob[11]) == 0);  // mtu 1500 LE
    Snapshot out;
    CHECK(parse(blob, out));
    CHECK(same(in, out));
}

TEST(a_sandbox_with_no_dns_servers_clears_the_resolver_flag_and_lists_none) {
    Snapshot in = wifi_fixed();
    in.resolver_ok = false;
    in.dns.clear();
    const std::string blob = serialize(in);
    CHECK((static_cast<unsigned char>(blob[1]) & 1) == 0);  // resolver flag clear
    CHECK((static_cast<unsigned char>(blob[1]) & 2) != 0);  // route dump still set
    Snapshot out;
    CHECK(parse(blob, out) && same(in, out));
    CHECK(out.dns.empty());
}

TEST(a_refused_route_dump_clears_the_flag_lists_no_route_and_guesses_no_gateway) {
    Snapshot in = wifi_fixed();
    in.route_dump_ok = false;
    in.routes.clear();
    const std::string blob = serialize(in);
    CHECK((static_cast<unsigned char>(blob[1]) & 2) == 0);  // route dump flag clear
    Snapshot out;
    CHECK(parse(blob, out) && same(in, out));
    CHECK(out.routes.empty());
}

TEST(the_fake_path_monitor_gives_one_source_to_the_snapshot_and_the_guest) {
    // The Mac session's fake path monitor feeds netlink's host source; the snapshot and the guest's own interfaces()
    // must then agree (one source). route_dump is allowed on the Mac, so the fake default route is in the snapshot.
    apkrt::network::start_fake_path_monitor();
    const apkrt::network::link::Snapshot snap = apkrt::network::link::read();
    CHECK(snap.have);
    CHECK(snap.iface == "wlan0");
    CHECK(snap.mtu == 1500);
    bool v4ok = false, def4 = false;
    for (const auto &a : snap.addresses)
        if (a.family == 4 && a.prefix == 24 && a.bytes == v4(192, 168, 7, 9)) v4ok = true;
    CHECK(v4ok);
    for (const auto &r : snap.routes)
        if (r.dest_family == 4 && r.dest_prefix == 0 && r.gateway == v4(192, 168, 7, 1)) def4 = true;
    CHECK(def4);
    bool guest_wlan0 = false;
    for (const auto &i : apkrt::netlink::interfaces())
        if (i.name == "wlan0")
            for (const auto &a : i.addresses)
                if (a.family == 2 && a.bytes == v4(192, 168, 7, 9)) guest_wlan0 = true;
    CHECK(guest_wlan0);
    apkrt::netlink::clear_fake_host();
}

TEST(the_fake_path_monitor_fixes_the_dns_so_the_snapshot_reads_no_host_server) {
    // Under the fake, the snapshot's DNS must be exactly the fake's two servers -- nothing of the developer's real
    // resolver (which would differ from machine to machine and leak the host's network into a test log).
    apkrt::network::start_fake_path_monitor();
    const apkrt::network::link::Snapshot snap = apkrt::network::link::read();
    CHECK(snap.resolver_ok);
    bool only_fake = snap.dns.size() == 2;
    for (const auto &d : snap.dns) {
        const bool is_fake = (d.family == 4 && d.bytes == v4(192, 168, 7, 1))
                             || (d.family == 6 && d.bytes == v6({0x20, 0x01, 0x0d, 0xb8}, 1));
        if (!is_fake) only_fake = false;
    }
    CHECK(only_fake);
    apkrt::netlink::clear_fake_host();
    apkrt::network::link::clear_fake_dns();
}

int main() { return apk::testing::run_all(); }
