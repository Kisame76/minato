// The host network probe's line (network_probe.cpp): counts and families only, so it is safe to log on the user's
// phone and network. These pin the line for known Readings and assert it carries no address, name or SSID -- the line
// may hold only digits and the words yes/no/ok/fail/denied, never a '.', ':' or '/' that an address or path would add.
#include "network_probe.h"

#include "check.h"

#include <string>

using apkrt::network::probe::format;
using apkrt::network::probe::Reading;

namespace {
/// The probe's line may contain only its keys and values: letters, digits, '=', ' '. An address ('.', ':', '/'), an
/// interface name or an SSID would bring other characters; none may appear.
bool counts_only(const std::string &line) {
    for (const char each : line) {
        const bool allowed = (each >= 'a' && each <= 'z') || (each >= 'A' && each <= 'Z') ||
                             (each >= '0' && each <= '9') || each == '=' || each == ' ' || each == '-';
        if (!allowed) return false;
    }
    return true;
}
}  // namespace

TEST(a_wifi_phone_reading_formats_as_counts_only) {
    Reading r;
    r.interfaces = 3;
    r.routable = 1;
    r.active = true;
    r.v4 = 1;
    r.v6 = 3;
    r.v6_global = 2;
    r.v6_link_local = 1;
    r.prefix_ok = true;
    r.private_v4 = true;
    r.mtu = 1500;
    r.dns = 2;
    r.dns_v4 = 1;
    r.dns_v6 = 1;
    r.resolver_ok = true;
    r.route_dump_ok = true;
    r.default_route = true;
    const std::string line = format(r);
    CHECK(line == "net-probe interfaces=3 routable=1 active=yes v4=1 v6=3 v6global=2 v6ll=1 prefixok=yes "
                  "privatev4=yes mtu=1500 dns=2 dnsv4=1 dnsv6=1 resolver=ok routedump=ok defaultroute=yes");
    CHECK(counts_only(line));
}

TEST(a_sandbox_that_refuses_the_route_dump_reads_as_denied_no_default_route) {
    Reading r;
    r.interfaces = 2;
    r.routable = 1;
    r.active = true;
    r.v4 = 1;
    r.prefix_ok = true;
    r.mtu = 1500;
    r.resolver_ok = true;
    r.route_dump_ok = false;  // the sandbox refused sysctl NET_RT_DUMP
    r.default_route = false;
    const std::string line = format(r);
    CHECK(line.find("routedump=denied") != std::string::npos);
    CHECK(line.find("defaultroute=no") != std::string::npos);
    CHECK(counts_only(line));
}

TEST(an_empty_reading_is_all_zero_and_still_counts_only) {
    const std::string line = format(Reading{});
    CHECK(line == "net-probe interfaces=0 routable=0 active=no v4=0 v6=0 v6global=0 v6ll=0 prefixok=no "
                  "privatev4=no mtu=0 dns=0 dnsv4=0 dnsv6=0 resolver=fail routedump=denied defaultroute=no");
    CHECK(counts_only(line));
}

int main() { return apk::testing::run_all(); }
