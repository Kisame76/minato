// The guest's /dev/socket/dnsproxyd (ADR-0054 step b): libnetd_client's commands answered by the rules of Android
// 15's DnsResolver module (DnsProxyListener.cpp, getaddrinfo.cpp and gethnamaddr.cpp of packages/modules/DnsResolver,
// read, not copied): the guest's /system/etc/hosts first, then the host's resolver.
#include "dns_proxy.h"
#include "sockets.h"

#include "vfs.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace apkrt::dns_proxy {
namespace {

// Linux's and bionic's numbers, which the guest sends and reads.
constexpr int linux_unspec = 0, linux_inet = 2, linux_inet6 = 10;
constexpr int sock_stream = 1, sock_dgram = 2, sock_raw = 3;
constexpr int ipproto_tcp = 6, ipproto_udp = 17;
constexpr int any = 0;
constexpr int ai_canonname = 0x2, ai_numericserv = 0x8, ai_addrconfig = 0x400;
constexpr int eai_again = 2, eai_fail = 4, eai_family = 5, eai_nodata = 7, eai_noname = 8, eai_service = 9,
              eai_socktype = 10;

// DnsProxyListener's response codes (ResponseCode.h).
constexpr int operation_failed = 400, query_result = 222, query_failed = 401, not_recognized = 500,
              parameter_error = 501;

/// libsysutils' FrameworkListener reads a command of at most this many bytes, with its NUL.
constexpr std::size_t command_size = 4096;

// --- The replies ------------------------------------------------------------------------------------------------

/// SocketClient::sendCode: the code's three digits and a NUL.
std::string code(int value) {
    std::string text = std::to_string(value);
    text += '\0';
    return text;
}

/// SocketClient::sendMsg without the command number: the code, a space, the text and a NUL.
std::string message(int value, std::string_view text) {
    std::string reply = std::to_string(value);
    reply += ' ';
    reply += text;
    reply += '\0';
    return reply;
}

void be32(std::string &out, std::uint32_t value) {
    const std::uint32_t big = htonl(value);
    out.append(reinterpret_cast<const char *>(&big), sizeof big);
}

/// sendLenAndData: a big-endian length and the bytes.
void with_length(std::string &out, std::string_view data) {
    be32(out, static_cast<std::uint32_t>(data.size()));
    out += data;
}

/// SocketClient::sendBinaryMsg of DnsProxyOperationFailed: the code, a big-endian length and the data.
std::string failure(std::string_view data) {
    std::string reply = code(query_failed);
    with_length(reply, data);
    return reply;
}

// --- Addresses and the hosts file ----------------------------------------------------------------------------

/// An address by its Linux family, its bytes (4 or 16) and an IPv6 scope.
struct Address {
    int family = linux_unspec;
    std::array<unsigned char, 16> bytes{};
    std::uint32_t scope = 0;

    std::size_t size() const { return family == linux_inet ? 4 : 16; }
    bool operator==(const Address &other) const {
        return family == other.family && std::memcmp(bytes.data(), other.bytes.data(), size()) == 0;
    }
};

/// inet_pton of `text` for `family`, or either family for AF_UNSPEC.
std::optional<Address> numeric(int family, const std::string &text) {
    Address address;
    const auto parses = [&](int host_family) {
        return ::inet_pton(host_family, text.c_str(), address.bytes.data()) == 1;
    };
    if ((family == linux_unspec || family == linux_inet6) && parses(AF_INET6)) {
        address.family = linux_inet6;
        return address;
    }
    if ((family == linux_unspec || family == linux_inet) && parses(AF_INET)) {
        address.family = linux_inet;
        return address;
    }
    return std::nullopt;
}

/// The guest's sockaddr of `address` with `port` (network order): Linux's sockaddr_in or sockaddr_in6.
std::string sockaddr_of(const Address &address, std::uint16_t port) {
    const auto family = static_cast<std::uint16_t>(address.family);
    std::string bytes(address.family == linux_inet ? 16 : 28, '\0');
    std::memcpy(bytes.data(), &family, 2);
    std::memcpy(bytes.data() + 2, &port, 2);
    if (address.family == linux_inet) {
        std::memcpy(bytes.data() + 4, address.bytes.data(), 4);
    } else {
        std::memcpy(bytes.data() + 8, address.bytes.data(), 16);
        std::memcpy(bytes.data() + 24, &address.scope, 4);
    }
    return bytes;
}

/// A line of /system/etc/hosts: the address and its names, the canonical one first.
struct HostsLine {
    Address address;
    std::vector<std::string> names;

    bool names_it(const std::string &name) const {
        for (const auto &own : names) {
            if (::strcasecmp(own.c_str(), name.c_str()) == 0) return true;
        }
        return false;
    }
};

/// How much of the hosts file the proxy reads: Android's has two lines, and a lookup must not read without end.
constexpr std::size_t hosts_file_cap = 64 * 1024;

/// The first hosts_file_cap bytes of the guest's /system/etc/hosts, opened beneath its mount (ADR-0052) and only as
/// a regular file, without waiting for a FIFO's writer; empty when there is none.
std::string hosts_text() {
    const auto location = vfs::locate("/system/etc/hosts");
    if (!location || location->below_a_file) return {};
    const int beneath = location->folder == AT_FDCWD ? 0 : O_RESOLVE_BENEATH;
    const int file = ::openat(location->folder, location->path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | beneath);
    if (file < 0) return {};
    std::string text;
    struct stat status {};
    if (::fstat(file, &status) == 0 && S_ISREG(status.st_mode)) {
        text.resize(hosts_file_cap);
        std::size_t filled = 0;
        while (filled < text.size()) {
            const auto got = ::read(file, text.data() + filled, text.size() - filled);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            filled += static_cast<std::size_t>(got);
        }
        text.resize(filled);
    }
    ::close(file);
    return text;
}

/// The guest's /system/etc/hosts, as the module's _gethtent and netbsd_gethostent_r read it: a line up to "#" or its
/// newline (a last line with neither is skipped), the address up to the first space or tab, then the names; a line
/// whose address does not parse is skipped.
std::vector<HostsLine> hosts_lines() {
    std::vector<HostsLine> lines;
    const std::string text = hosts_text();
    for (std::size_t position = 0; position < text.size();) {
        const auto newline = text.find('\n', position);
        std::string line = text.substr(position, newline == std::string::npos ? std::string::npos : newline - position);
        position = newline == std::string::npos ? text.size() : newline + 1;
        const auto comment = line.find('#');
        if (comment == std::string::npos && newline == std::string::npos) continue;
        if (comment != std::string::npos) line.resize(comment);
        const auto blank = line.find_first_of(" \t");
        if (blank == std::string::npos) continue;
        const auto address = numeric(linux_unspec, line.substr(0, blank));
        if (!address) continue;
        HostsLine entry{.address = *address, .names = {}};
        for (std::size_t at = blank; at < line.size();) {
            const auto start = line.find_first_not_of(" \t", at);
            if (start == std::string::npos) break;
            const auto stop = std::min(line.find_first_of(" \t", start), line.size());
            entry.names.push_back(line.substr(start, stop - start));
            at = stop;
        }
        if (!entry.names.empty()) lines.push_back(std::move(entry));
    }
    return lines;
}

// --- The host's resolver -------------------------------------------------------------------------------------

/// What the host's getaddrinfo found for a name: its addresses in its order, each once, and the canonical name; or
/// the module's error for none.
struct HostAnswer {
    int error = 0;
    std::vector<Address> addresses;
    std::string canonical;
};

HostAnswer host_addresses(const std::string &name, int family, int flags) {
    // Without INTERNET, Android's answer to a blocked uid (DnsProxyListener: EAI_FAIL), and no lookup by the host.
    if (!sockets::internet()) return {.error = eai_fail};
    addrinfo hints{};
    hints.ai_family = family == linux_inet ? AF_INET : family == linux_inet6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;  // one entry for each address
    hints.ai_flags = AI_CANONNAME | ((flags & ai_addrconfig) != 0 ? AI_ADDRCONFIG : 0);
    addrinfo *result = nullptr;
    HostAnswer answer;
    if (const int error = ::getaddrinfo(name.c_str(), nullptr, &hints, &result); error != 0) {
        answer.error = error == EAI_NONAME ? eai_nodata : error == EAI_AGAIN ? eai_again : eai_fail;
        return answer;
    }
    for (const addrinfo *entry = result; entry != nullptr; entry = entry->ai_next) {
        Address address;
        if (entry->ai_family == AF_INET) {
            address.family = linux_inet;
            std::memcpy(address.bytes.data(), &reinterpret_cast<const sockaddr_in *>(entry->ai_addr)->sin_addr, 4);
        } else if (entry->ai_family == AF_INET6) {
            const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
            address.family = linux_inet6;
            std::memcpy(address.bytes.data(), &in6->sin6_addr, 16);
            address.scope = in6->sin6_scope_id;
        } else {
            continue;
        }
        bool seen = false;
        for (const auto &known : answer.addresses) seen = seen || known == address;
        if (!seen) answer.addresses.push_back(address);
    }
    answer.canonical = result != nullptr && result->ai_canonname != nullptr ? result->ai_canonname : name;
    ::freeaddrinfo(result);
    if (answer.addresses.empty()) answer.error = eai_nodata;
    return answer;
}

/// The name the host's resolver gives `address` (getnameinfo with NI_NAMEREQD), or nothing.
std::optional<std::string> host_name_of(const Address &address) {
    if (!sockets::internet()) return std::nullopt;  // as host_addresses
    sockaddr_storage storage{};
    socklen_t length;
    if (address.family == linux_inet) {
        auto &in = reinterpret_cast<sockaddr_in &>(storage);
        in.sin_len = sizeof in;
        in.sin_family = AF_INET;
        std::memcpy(&in.sin_addr, address.bytes.data(), 4);
        length = sizeof in;
    } else {
        auto &in6 = reinterpret_cast<sockaddr_in6 &>(storage);
        in6.sin6_len = sizeof in6;
        in6.sin6_family = AF_INET6;
        std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
        length = sizeof in6;
    }
    char name[NI_MAXHOST];
    if (::getnameinfo(reinterpret_cast<const sockaddr *>(&storage), length, name, sizeof name, nullptr, 0,
                      NI_NAMEREQD) != 0) {
        return std::nullopt;
    }
    return std::string(name);
}

// --- getaddrinfo ---------------------------------------------------------------------------------------------

struct Hints {
    int flags = 0, family = linux_unspec, socktype = any, protocol = any;
};

/// The module's explore_options: the combinations getaddrinfo answers, in its order; a raw socket's type is no
/// wildcard (e_wild 0x05), so raw entries come only when asked.
struct Explore {
    int family, socktype, protocol;
    bool wild_socktype;
};
constexpr Explore explore[] = {
    {linux_inet6, sock_dgram, ipproto_udp, true},  {linux_inet6, sock_stream, ipproto_tcp, true},
    {linux_inet6, sock_raw, any, false},           {linux_inet, sock_dgram, ipproto_udp, true},
    {linux_inet, sock_stream, ipproto_tcp, true},  {linux_inet, sock_raw, any, false},
    {linux_unspec, sock_dgram, ipproto_udp, true}, {linux_unspec, sock_stream, ipproto_tcp, true},
    {linux_unspec, sock_raw, any, false},
};

bool match(int requested, int offered, bool wild) {
    return requested == offered || (wild && (requested == any || offered == any));
}

/// str2number: a decimal number in bionic's int, or -1 for none; one above INT_MAX is negative there, so no number.
long str2number(const char *text) {
    if (*text == '\0') return -1;
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || value > UINT_MAX) return -1;
    return static_cast<int>(static_cast<unsigned>(value));
}

/// get_port: the port of `service` (network order, 0 for none) for an entry of `family` and `socktype`, or the error.
int port_of(const char *service, int family, int socktype, int flags, std::uint16_t &port) {
    port = 0;
    if (service == nullptr || (family != linux_inet && family != linux_inet6)) return 0;
    if (socktype == sock_raw) return eai_service;
    if (socktype != sock_dgram && socktype != sock_stream && socktype != any) return eai_socktype;
    if (const long number = str2number(service); number >= 0) {
        if (number > 65535) return eai_service;
        port = htons(static_cast<std::uint16_t>(number));
        return 0;
    }
    if ((flags & ai_numericserv) != 0) return eai_noname;
    // The host's services (iOS's /etc/services, the same IANA names as Android's built-in table, as far as apps use
    // them); getservbyname has one buffer for the process.
    // Never destroyed: a detached converse thread may lock it in getaddrinfo during a host test exit; a
    // static mutex's dtor could run first -> EINVAL. The device uses _exit (no static dtors). See stall_watchdog.cpp.
    static std::mutex &services = *new std::mutex;
    const std::lock_guard lock(services);
    const servent *entry =
        ::getservbyname(service, socktype == sock_dgram ? "udp" : socktype == sock_stream ? "tcp" : nullptr);
    if (entry == nullptr) return eai_service;
    port = static_cast<std::uint16_t>(entry->s_port);
    return 0;
}

struct Result {
    int flags, family, socktype, protocol;
    Address address;
    std::uint16_t port;
    std::string canonical;
};

/// resolv_getaddrinfo for a name: each combination of explore that the hints allow, in its order, with what the hosts
/// file has for the name, else what the host's resolver has; the last error when none has anything.
int addresses_for(const std::string &name, const char *service, const Hints &hints, std::vector<Result> &results) {
    if (hints.family != linux_unspec && hints.family != linux_inet && hints.family != linux_inet6) return eai_family;
    if (hints.socktype != any && hints.socktype != sock_stream && hints.socktype != sock_dgram &&
        hints.socktype != sock_raw) {
        return eai_socktype;
    }
    const auto lines = hosts_lines();
    std::optional<HostAnswer> resolved;  // asked once, at the first combination the file has nothing for
    int error = 0;
    for (const auto &option : explore) {
        if (hints.family != option.family || !match(hints.socktype, option.socktype, option.wild_socktype) ||
            !match(hints.protocol, option.protocol, true)) {
            continue;
        }
        const int socktype = hints.socktype == any ? option.socktype : hints.socktype;
        const int protocol = hints.protocol == any ? option.protocol : hints.protocol;
        std::uint16_t port;
        if (const int refused = port_of(service, linux_inet, socktype, hints.flags, port); refused != 0) {
            error = refused;  // get_portmatch
            continue;
        }
        std::vector<Result> found;
        for (const auto &line : lines) {
            if (!line.names_it(name) || (hints.family != linux_unspec && line.address.family != hints.family)) continue;
            found.push_back({hints.flags, line.address.family, socktype, protocol, line.address, 0,
                             (hints.flags & ai_canonname) != 0 ? line.names.front() : std::string()});
        }
        if (found.empty()) {
            if (!resolved) resolved = host_addresses(name, hints.family, hints.flags);
            if (resolved->error != 0) {
                error = resolved->error;
                continue;
            }
            for (const auto &address : resolved->addresses) {
                const bool first = found.empty();
                found.push_back({hints.flags, address.family, socktype, protocol, address, 0,
                                 first && (hints.flags & ai_canonname) != 0 ? resolved->canonical : std::string()});
            }
        }
        for (auto &result : found) {
            if (const int refused = port_of(service, result.family, socktype, hints.flags, result.port); refused != 0) {
                error = refused;
                found.clear();
                break;
            }
        }
        results.insert(results.end(), found.begin(), found.end());
    }
    if (!results.empty()) return 0;
    return error == 0 ? eai_fail : error;
}

/// Parses a decimal int as android-base's ParseInt, the whole text.
std::optional<long> parse_int(const std::string &text) {
    if (text.empty()) return std::nullopt;
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || value < INT_MIN || value > INT_MAX) return std::nullopt;
    return value;
}

/// A netid as netd's ParseUint reads it into an unsigned (android-base/parseint.h): whitespace skipped, no sign,
/// hexadecimal after 0x, else decimal, the whole text a number of at most 2^32 - 1.
std::optional<std::uint32_t> parse_netid(const std::string &text) {
    const char *start = text.c_str();
    while (std::isspace(static_cast<unsigned char>(*start))) ++start;
    if (*start == '-') return std::nullopt;
    const int base = start[0] == '0' && (start[1] == 'x' || start[1] == 'X') ? 16 : 10;
    char *end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(start, &end, base);
    if (errno != 0 || end == start || *end != '\0' || value > UINT32_MAX) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

std::string getaddrinfo_reply(const std::vector<std::string> &argv) {
    if (argv.size() != 8) {
        return message(parameter_error,
                       "GetAddrInfoCmd::runCommand: invalid number of arguments: " + std::to_string(argv.size()));
    }
    std::array<long, 4> fields{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto value = parse_int(argv[3 + index]);
        if (!value) return message(parameter_error, "GetAddrInfoCmd::runCommand: ");
        fields[index] = *value;
    }
    if (!parse_netid(argv[7])) {
        return message(parameter_error, "GetAddrInfoCmd::runCommand: ");
    }
    Hints hints;  // none when all four are -1: AF_UNSPEC and wildcards
    if (fields[0] != -1 || fields[1] != -1 || fields[2] != -1 || fields[3] != -1) {
        hints = {static_cast<int>(fields[0]), static_cast<int>(fields[1]), static_cast<int>(fields[2]),
                 static_cast<int>(fields[3])};
    }
    const std::string service = argv[2];
    std::vector<Result> results;
    const int error = addresses_for(argv[1], service == "^" ? nullptr : service.c_str(), hints, results);
    if (error != 0) {
        const std::int32_t value = error;
        return failure(std::string_view(reinterpret_cast<const char *>(&value), sizeof value));
    }
    std::string reply = code(query_result);
    for (const auto &result : results) {
        be32(reply, 1);
        be32(reply, static_cast<std::uint32_t>(result.flags));
        be32(reply, static_cast<std::uint32_t>(result.family));
        be32(reply, static_cast<std::uint32_t>(result.socktype));
        be32(reply, static_cast<std::uint32_t>(result.protocol));
        with_length(reply, sockaddr_of(result.address, result.port));
        with_length(reply, result.canonical.empty() ? std::string() : result.canonical + '\0');
    }
    be32(reply, 0);
    return reply;
}

// --- gethostbyname and gethostbyaddr -------------------------------------------------------------------------

struct HostEntry {
    std::string name;
    std::vector<std::string> aliases;
    int family = linux_inet;
    std::vector<Address> addresses;
};

/// DnsProxyListener's sendhostent: each address in 16 bytes, an IPv4 one too.
std::string hostent_reply(const HostEntry &host) {
    std::string reply = code(query_result);
    with_length(reply, host.name + '\0');
    for (const auto &alias : host.aliases) with_length(reply, alias + '\0');
    be32(reply, 0);
    be32(reply, static_cast<std::uint32_t>(host.family));
    be32(reply, host.family == linux_inet ? 4 : 16);
    for (const auto &address : host.addresses) {
        with_length(reply, std::string_view(reinterpret_cast<const char *>(address.bytes.data()), 16));
    }
    be32(reply, 0);
    return reply;
}

/// gethostbyname_internal_real's test for a name it answers without a lookup: digits and dots, or an IPv6 text,
/// without a dot at the end.
bool all_numeric(const std::string &name) {
    const auto only = [&](auto allowed) {
        for (const char letter : name) {
            if (!allowed(static_cast<unsigned char>(letter))) return false;
        }
        return name.back() != '.';
    };
    if (name.empty()) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (std::isdigit(first) && only([](unsigned char letter) { return std::isdigit(letter) || letter == '.'; })) {
        return true;
    }
    return ((std::isxdigit(first) && name.find(':') != std::string::npos) || first == ':') &&
           only([](unsigned char letter) { return std::isxdigit(letter) || letter == ':' || letter == '.'; });
}

/// resolv_gethostbyname: a numeric name as it is; the hosts file's lines for the name in `family`, the first one's
/// names and every line's address; else the host's resolver.
std::optional<HostEntry> host_by_name(const std::string &name, int family) {
    if (family != linux_inet && family != linux_inet6) return std::nullopt;
    if (all_numeric(name)) {
        const auto address = numeric(family, name);
        if (!address) return std::nullopt;
        return HostEntry{.name = name, .aliases = {}, .family = family, .addresses = {*address}};
    }
    HostEntry host{.name = {}, .aliases = {}, .family = family, .addresses = {}};
    constexpr std::size_t most_addresses = 35;  // MAXADDRS
    for (const auto &line : hosts_lines()) {
        if (line.address.family != family || !line.names_it(name) || host.addresses.size() >= most_addresses) continue;
        if (host.addresses.empty()) {
            host.name = line.names.front();
            host.aliases.assign(line.names.begin() + 1, line.names.end());
        }
        host.addresses.push_back(line.address);
    }
    if (!host.addresses.empty()) return host;
    const auto resolved = host_addresses(name, family, 0);
    if (resolved.error != 0) return std::nullopt;
    return HostEntry{.name = resolved.canonical, .aliases = {}, .family = family, .addresses = resolved.addresses};
}

/// resolv_gethostbyaddr: nothing for an IPv6 link-local or site-local address; an IPv4-mapped or -compatible one as
/// its IPv4 address; the hosts file's first line with it, else the host's resolver.
std::optional<HostEntry> host_by_address(Address address, long length) {
    if (address.family == linux_inet6 && length == 16) {
        in6_addr bytes;
        std::memcpy(&bytes, address.bytes.data(), 16);
        if (IN6_IS_ADDR_LINKLOCAL(&bytes) || IN6_IS_ADDR_SITELOCAL(&bytes)) return std::nullopt;
        if (IN6_IS_ADDR_V4MAPPED(&bytes) || IN6_IS_ADDR_V4COMPAT(&bytes)) {
            std::memmove(address.bytes.data(), address.bytes.data() + 12, 4);
            std::memset(address.bytes.data() + 4, 0, 12);
            address.family = linux_inet;
            length = 4;
        }
    }
    if (length != static_cast<long>(address.size())) return std::nullopt;
    for (const auto &line : hosts_lines()) {
        if (line.address == address) {
            return HostEntry{.name = line.names.front(),
                             .aliases = std::vector<std::string>(line.names.begin() + 1, line.names.end()),
                             .family = address.family,
                             .addresses = {address}};
        }
    }
    const auto name = host_name_of(address);
    if (!name) return std::nullopt;
    return HostEntry{.name = *name, .aliases = {}, .family = address.family, .addresses = {address}};
}

std::string gethostbyname_reply(const std::vector<std::string> &argv) {
    const std::string error = "GetHostByNameCmd::runCommand: ";
    if (argv.size() != 4) {
        return message(parameter_error, error + "invalid number of arguments: " + std::to_string(argv.size()));
    }
    const auto netid = parse_netid(argv[1]);
    const auto family = parse_int(argv[3]);
    if (!netid || !family) return message(parameter_error, error);
    const auto host = host_by_name(argv[2], static_cast<int>(*family));
    return host ? hostent_reply(*host) : failure({});
}

std::string gethostbyaddr_reply(const std::vector<std::string> &argv) {
    const std::string error = "GetHostByAddrCmd::runCommand: ";
    if (argv.size() != 5) {
        return message(parameter_error, error + "invalid number of arguments: " + std::to_string(argv.size()));
    }
    const auto length = parse_int(argv[2]);
    const auto family = parse_int(argv[3]);
    const auto netid = parse_netid(argv[4]);
    if (!length || !family || !netid) return message(parameter_error, error);
    const auto address = *family == linux_inet || *family == linux_inet6
                             ? numeric(static_cast<int>(*family), argv[1])
                             : std::nullopt;
    if (!address) return message(operation_failed, error + "inet_pton(\"" + argv[1] + "\") failed");
    const auto host = host_by_address(*address, *length);
    return host ? hostent_reply(*host) : failure({});
}

// --- The connections -----------------------------------------------------------------------------------------

/// A runtime thread takes no signal: the guest's go to its own threads.
void block_signals() {
    sigset_t all;
    sigfillset(&all);
    ::pthread_sigmask(SIG_BLOCK, &all, nullptr);
}

/// Sends `bytes` whole, without SIGPIPE: Darwin raises a socket's SIGPIPE for the process, not the thread, so a
/// blocked mask does not keep it from the guest's threads, and SO_NOSIGPIPE cannot be set on a connection whose client
/// is gone before the accept.
bool write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

/// One listener's state, which its accept thread and its connections' threads share.
struct Server {
    int listener = -1;
    std::function<void()> closed;
    Limits limits;
    std::atomic<unsigned> live{0};  // connections with a thread
};

/// Starts `routine` with `argument` on a detached thread; false when none can be made, which a std::thread would
/// answer with an exception, and in a guest's syscall with std::terminate.
bool start(void *(*routine)(void *), void *argument) {
    pthread_attr_t attributes;
    if (::pthread_attr_init(&attributes) != 0) return false;
    ::pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const bool started = ::pthread_create(&thread, &attributes, routine, argument) == 0;
    ::pthread_attr_destroy(&attributes);
    return started;
}

/// One connection: its commands in turn, each up to its NUL, until the guest closes it or sends nothing for the idle
/// time (SO_RCVTIMEO). A command of command_size bytes or more gets "500" and its rest is skipped to its NUL, as
/// FrameworkListener's mSkipToNextNullByte does.
void converse(int client) {
    std::string pending;
    bool skipping = false;
    char buffer[command_size];
    for (;;) {
        const ssize_t got = ::read(client, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;  // the end, an error, or EAGAIN after the idle time
        pending.append(buffer, static_cast<std::size_t>(got));
        bool open = true;
        for (std::size_t end; open && (end = pending.find('\0')) != std::string::npos;) {
            if (!skipping) open = write_all(client, answer(std::string_view(pending).substr(0, end)));
            skipping = false;
            pending.erase(0, end + 1);
        }
        if (open && !skipping && pending.size() >= command_size) {
            open = write_all(client, message(not_recognized, "Command too large for buffer"));
            skipping = true;
        }
        if (skipping) pending.clear();
        if (!open) break;
    }
    ::close(client);
}

struct Conversation {
    int client;
    std::shared_ptr<Server> server;
};

void *converse_on_thread(void *argument) {
    block_signals();
    const std::unique_ptr<Conversation> conversation(static_cast<Conversation *>(argument));
    converse(conversation->client);
    conversation->server->live.fetch_sub(1);
    return nullptr;
}

/// The accept loop: a connection beyond the limit is closed at once; one that cannot get a thread too.
void *accept_on_thread(void *argument) {
    block_signals();
    const std::unique_ptr<std::shared_ptr<Server>> held(static_cast<std::shared_ptr<Server> *>(argument));
    const auto server = *held;
    for (;;) {
        const int client = ::accept(server->listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // until descriptors or buffers come back
                continue;
            }
            break;  // EBADF, EINVAL: the listener is gone
        }
        if (server->live.fetch_add(1) >= server->limits.connections) {
            server->live.fetch_sub(1);
            ::close(client);
            continue;
        }
        const auto idle = std::chrono::duration_cast<std::chrono::microseconds>(server->limits.idle).count();
        const timeval timeout{.tv_sec = static_cast<time_t>(idle / 1'000'000),
                              .tv_usec = static_cast<suseconds_t>(idle % 1'000'000)};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        auto *conversation = new Conversation{client, server};
        if (!start(converse_on_thread, conversation)) {
            delete conversation;
            ::close(client);
            server->live.fetch_sub(1);
        }
    }
    ::close(server->listener);
    if (server->closed) server->closed();
    return nullptr;
}

}  // namespace

std::string answer(std::string_view command) {
    // FrameworkListener's arguments, apart by spaces; bionic's commands have no quotes.
    std::vector<std::string> argv;
    for (std::size_t position = 0; position <= command.size();) {
        const auto space = std::min(command.find(' ', position), command.size());
        argv.emplace_back(command.substr(position, space - position));
        position = space + 1;
    }
    if (argv.front() == "getaddrinfo") return getaddrinfo_reply(argv);
    if (argv.front() == "gethostbyname") return gethostbyname_reply(argv);
    if (argv.front() == "gethostbyaddr") return gethostbyaddr_reply(argv);
    return message(not_recognized, "Command not recognized");
}

bool serve(int listener, std::function<void()> closed, Limits limits) {
    auto server = std::make_shared<Server>();
    server->listener = listener;
    server->closed = std::move(closed);
    server->limits = limits;
    auto *held = new std::shared_ptr<Server>(std::move(server));
    if (start(accept_on_thread, held)) return true;
    delete held;
    ::close(listener);
    return false;
}

}  // namespace apkrt::dns_proxy
