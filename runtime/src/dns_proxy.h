// The guest's /dev/socket/dnsproxyd (ADR-0054 step b). On Android 15, bionic's resolver functions send their
// lookups through libnetd_client to the DnsResolver module, which answers them there (DnsProxyListener.cpp). Here a
// thread of the runtime's answers them: from the guest's /system/etc/hosts first, as the module reads it before DNS,
// then from the host's resolver, so that iOS's DNS settings, VPN, encrypted DNS, NAT64 and .local apply.
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace apkrt::dns_proxy {

/// The guest's name of the proxy, where libnetd_client's dns_open_proxy connects.
inline constexpr std::string_view guest_name = "/dev/socket/dnsproxyd";

/// The reply to one command (its text without the NUL that ends it), as DnsProxyListener and libsysutils'
/// SocketClient write it: "222" with its NUL and the answer for a lookup that found something; "401" with its NUL,
/// a big-endian length and that many bytes for one that did not (getaddrinfo's error, nothing for the others);
/// "500" or "501", a text and a NUL for what is no command or has the wrong arguments.
std::string answer(std::string_view command);

/// How serve keeps its connections: at most `connections` at once, a further one closed at its accept; and each closed
/// once no command has come for `idle` (SO_RCVTIMEO), since a client that sends nothing would keep its thread.
struct Limits {
    unsigned connections = 256;
    std::chrono::milliseconds idle{10'000};
};

/// Answers the connections of the listening socket `listener` on threads of their own, one per connection, as a
/// lookup can take seconds; takes `listener`. While descriptors or buffers run out (EMFILE, ENFILE, ENOBUFS, ENOMEM)
/// accept tries again every 100 ms; once the listener is gone (EBADF, EINVAL) its thread closes it, calls `closed`
/// and ends. Returns false, with `listener` closed, when no thread could be started.
bool serve(int listener, std::function<void()> closed = {}, Limits limits = {});

}  // namespace apkrt::dns_proxy
