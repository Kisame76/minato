// Checks the guest's /dev/socket/dnsproxyd (ADR-0054 step b): the replies to libnetd_client's commands in
// DnsProxyListener's bytes, the guest's /system/etc/hosts before the host's resolver, the errors, and connections
// that end before their reply. fixtures/suite/socket-test checks what bionic makes of the replies against Android 15.
#include "APKRuntime/apkrt.h"
#include "dns_proxy.h"
#include "vfs.h"

#include "check.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using apkrt::dns_proxy::answer;

constexpr int linux_inet = 2, linux_inet6 = 10;
constexpr int eai_nodata = 7;  // bionic's

/// A guest /system whose etc/hosts holds `hosts`, mounted until the end.
struct GuestSystem {
    std::string host;

    explicit GuestSystem(const std::string &hosts) {
        char pattern[] = "/tmp/apkrt-dns-XXXXXX";
        host = ::mkdtemp(pattern);
        CHECK(::mkdir((host + "/etc").c_str(), 0755) == 0);
        std::ofstream(host + "/etc/hosts") << hosts;
        apkrt::vfs::reset();
        apkrt::vfs::mount("/system", host);
    }
    ~GuestSystem() {
        apkrt::vfs::reset();
        static_cast<void>(std::system(("rm -rf '" + host + "'").c_str()));
    }
};

/// Android 15's /system/etc/hosts.
const std::string android_hosts = "127.0.0.1       localhost\n::1             ip6-localhost\n";

/// Reads a reply as libnetd_client's callers do.
struct Reader {
    std::string_view bytes;
    bool ok = true;

    std::string take(std::size_t size) {
        if (bytes.size() < size) {
            ok = false;
            return {};
        }
        std::string result(bytes.substr(0, size));
        bytes.remove_prefix(size);
        return result;
    }
    std::int32_t be32() {
        const auto data = take(4);
        if (!ok) return 0;
        std::uint32_t value;
        std::memcpy(&value, data.data(), 4);
        return static_cast<std::int32_t>(ntohl(value));
    }
    std::string with_length() { return take(static_cast<std::size_t>(be32())); }
};

struct Entry {
    int flags = 0, family = 0, socktype = 0, protocol = 0;
    std::string address;  // as text
    unsigned port = 0;
    std::size_t length = 0;
    std::string canonical;
};

/// A getaddrinfo reply's entries, or nullopt when it is not "222" or does not parse.
std::optional<std::vector<Entry>> entries_of(const std::string &reply) {
    Reader reader{reply};
    if (reader.take(4) != std::string("222", 4)) return std::nullopt;
    std::vector<Entry> entries;
    while (reader.ok) {
        if (reader.be32() == 0) break;
        Entry entry;
        entry.flags = reader.be32();
        entry.family = reader.be32();
        entry.socktype = reader.be32();
        entry.protocol = reader.be32();
        const auto address = reader.with_length();
        entry.length = address.size();
        char text[INET6_ADDRSTRLEN] = "";
        std::uint16_t family = 0, port = 0;
        if (address.size() >= 4) {
            std::memcpy(&family, address.data(), 2);
            std::memcpy(&port, address.data() + 2, 2);
        }
        if (family == linux_inet && address.size() == 16) ::inet_ntop(AF_INET, address.data() + 4, text, sizeof text);
        if (family == linux_inet6 && address.size() == 28) ::inet_ntop(AF_INET6, address.data() + 8, text, sizeof text);
        entry.address = text;
        entry.port = ntohs(port);
        const auto canonical = reader.with_length();
        entry.canonical = canonical.empty() ? "" : canonical.substr(0, canonical.size() - 1);
        if (!canonical.empty() && canonical.back() != '\0') return std::nullopt;
        entries.push_back(entry);
    }
    if (!reader.ok || !reader.bytes.empty()) return std::nullopt;
    return entries;
}

struct Host {
    std::string name;
    std::vector<std::string> aliases;
    int type = 0, length = 0;
    std::vector<std::string> addresses;  // the first `length` bytes of each, as text
    std::vector<std::size_t> sizes;      // each address's length in the reply
};

/// A gethostbyname or gethostbyaddr reply, or nullopt when it is not "222" or does not parse.
std::optional<Host> host_of(const std::string &reply) {
    Reader reader{reply};
    if (reader.take(4) != std::string("222", 4)) return std::nullopt;
    Host host;
    auto name = reader.with_length();
    if (name.empty() || name.back() != '\0') return std::nullopt;
    host.name = name.substr(0, name.size() - 1);
    for (auto alias = reader.with_length(); reader.ok && !alias.empty(); alias = reader.with_length()) {
        host.aliases.push_back(alias.substr(0, alias.size() - 1));
    }
    host.type = reader.be32();
    host.length = reader.be32();
    for (auto address = reader.with_length(); reader.ok && !address.empty(); address = reader.with_length()) {
        char text[INET6_ADDRSTRLEN] = "";
        ::inet_ntop(host.type == linux_inet ? AF_INET : AF_INET6, address.data(), text, sizeof text);
        host.addresses.emplace_back(text);
        host.sizes.push_back(address.size());
    }
    if (!reader.ok || !reader.bytes.empty()) return std::nullopt;
    return host;
}

std::string failure(std::string_view data) {
    std::string reply("401", 4);
    const std::uint32_t length = htonl(static_cast<std::uint32_t>(data.size()));
    reply.append(reinterpret_cast<const char *>(&length), 4);
    reply += data;
    return reply;
}

std::string int_bytes(std::int32_t value) {
    return std::string(reinterpret_cast<const char *>(&value), 4);
}

TEST(getaddrinfo_answers_a_name_of_the_hosts_file_as_android_15) {
    // Without hints: a datagram and a stream entry, no raw one, as the emulator's socket-test shows.
    const GuestSystem system(android_hosts);
    const auto entries = entries_of(answer("getaddrinfo localhost ^ -1 -1 -1 -1 0"));
    CHECK(entries && entries->size() == 2);
    if (!entries || entries->size() != 2) return;
    const auto &dgram = (*entries)[0], &stream = (*entries)[1];
    CHECK(dgram.family == linux_inet && dgram.socktype == 2 && dgram.protocol == 17 && dgram.flags == 0);
    CHECK(stream.family == linux_inet && stream.socktype == 1 && stream.protocol == 6);
    CHECK(dgram.address == "127.0.0.1" && dgram.port == 0 && dgram.length == 16 && dgram.canonical.empty());
}

TEST(getaddrinfo_takes_the_hints_and_the_service) {
    const GuestSystem system(android_hosts);
    auto entries = entries_of(answer("getaddrinfo localhost 80 0 0 0 0 0"));
    CHECK(entries && entries->size() == 2 && (*entries)[0].port == 80 && (*entries)[1].port == 80);
    entries = entries_of(answer("getaddrinfo localhost http 0 0 1 0 0"));
    CHECK(entries && entries->size() == 1 && (*entries)[0].port == 80 && (*entries)[0].socktype == 1);
    // A name in any case, with its canonical name from the file; the flags come back in each entry.
    entries = entries_of(answer("getaddrinfo LocalHost ^ 2 2 2 0 0"));
    CHECK(entries && entries->size() == 1 && (*entries)[0].canonical == "localhost" && (*entries)[0].flags == 2);
    entries = entries_of(answer("getaddrinfo ip6-localhost ^ 0 0 1 0 0"));
    CHECK(entries && entries->size() == 1 && (*entries)[0].family == linux_inet6 && (*entries)[0].address == "::1" &&
          (*entries)[0].length == 28);
    // Raw sockets only when asked, and without a service.
    entries = entries_of(answer("getaddrinfo localhost ^ 0 2 3 0 0"));
    CHECK(entries && entries->size() == 1 && (*entries)[0].socktype == 3 && (*entries)[0].protocol == 0);
    CHECK(answer("getaddrinfo localhost 80 0 2 3 0 0") == failure(int_bytes(9)));  // EAI_SERVICE
    CHECK(answer("getaddrinfo localhost nosuchservice 0 0 1 0 0") == failure(int_bytes(9)));
}

TEST(the_hosts_file_comes_before_the_hosts_resolver) {
    // The Mac's and the iPhone's /etc/hosts also give ::1 for localhost; the guest's file has 127.0.0.1 alone.
    {
        const GuestSystem system(android_hosts);
        const auto entries = entries_of(answer("getaddrinfo localhost ^ 0 0 1 0 0"));
        CHECK(entries && entries->size() == 1 && (*entries)[0].address == "127.0.0.1");
    }
    // Without the name in the file, the host's resolver answers, here from the host's own hosts file.
    const GuestSystem system("10.1.2.3 elsewhere\n");
    const auto entries = entries_of(answer("getaddrinfo localhost ^ 0 0 1 0 0"));
    bool loopback = false;
    for (const auto &entry : entries.value_or(std::vector<Entry>{})) {
        loopback = loopback || entry.address == "127.0.0.1" || entry.address == "::1";
    }
    CHECK(loopback);
}

TEST(the_hosts_file_reads_as_androids_resolver_reads_it) {
    // A comment ends a line's names, a last line without its newline is skipped (_gethtent), and every line with the
    // name counts, for getaddrinfo and gethostbyname. Names the file does not give go to the host's resolver, which
    // resolves no .invalid name.
    const GuestSystem system("127.0.0.1 localhost # hidden.invalid\n10.0.0.1 multi\n10.0.0.2 multi\n"
                             "10.9.8.7 only-here.invalid\n10.0.0.3 tail.invalid");
    // A name in any case, here one that only this file has.
    const auto capital = entries_of(answer("getaddrinfo ONLY-HERE.INVALID ^ 0 0 1 0 0"));
    CHECK(capital && capital->size() == 1 && (*capital)[0].address == "10.9.8.7");
    CHECK(answer("getaddrinfo hidden.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
    CHECK(answer("getaddrinfo tail.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
    const auto entries = entries_of(answer("getaddrinfo multi ^ 0 0 1 0 0"));
    CHECK(entries && entries->size() == 2 && (*entries)[0].address == "10.0.0.1" &&
          (*entries)[1].address == "10.0.0.2");
    const auto host = host_of(answer("gethostbyname 0 multi 2"));
    CHECK(host && (host->addresses == std::vector<std::string>{"10.0.0.1", "10.0.0.2"}));
}

// The hosts file is opened beneath /system's mount, as a regular file, and read up to 64 KiB (ADR-0054 step c2a, the
// review of step b): a link out of the mount, a FIFO that would block the proxy's thread, and the end of a huge file
// count as no hosts file, as bionic finds none in its place.
TEST(the_hosts_file_is_read_only_as_a_regular_file_beneath_its_mount) {
    char pattern[] = "/tmp/apkrt-dns-outside-XXXXXX";
    const std::string outside = ::mkdtemp(pattern);
    std::ofstream(outside + "/hosts") << "10.9.8.7 outside.invalid\n";
    {
        const GuestSystem system("");
        CHECK(::unlink((system.host + "/etc/hosts").c_str()) == 0);
        CHECK(::symlink((outside + "/hosts").c_str(), (system.host + "/etc/hosts").c_str()) == 0);
        CHECK(answer("getaddrinfo outside.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
    }
    static_cast<void>(std::system(("rm -rf '" + outside + "'").c_str()));
    {
        const GuestSystem system("");
        const auto fifo = system.host + "/etc/hosts";
        CHECK(::unlink(fifo.c_str()) == 0 && ::mkfifo(fifo.c_str(), 0644) == 0);
        auto reply = std::async(std::launch::async, [] { return answer("getaddrinfo nothing.invalid ^ 0 0 1 0 0"); });
        if (reply.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            apk::testing::fail(std::source_location::current(), "the proxy waits on a FIFO: ", fifo.c_str());
            ::close(::open(fifo.c_str(), O_WRONLY | O_NONBLOCK));  // lets the reader go
        }
        CHECK(reply.get() == failure(int_bytes(eai_nodata)));
        // Nor a FIFO with a writer and lines in it: it is no regular file.
        const int writer = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK);
        CHECK(writer >= 0);
        const std::string_view line = "10.9.8.7 fifo.invalid\n";
        CHECK(::write(writer, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
        CHECK(answer("getaddrinfo fifo.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
        ::close(writer);
    }
    std::string huge = "127.0.0.9 early.invalid\n";
    while (huge.size() < 70 * 1024) huge += "# padding to beyond the 64 KiB the proxy reads of the file\n";
    huge += "127.0.0.10 late.invalid\n";
    const GuestSystem system(huge);
    const auto early = entries_of(answer("getaddrinfo early.invalid ^ 0 0 1 0 0"));
    CHECK(early && early->size() == 1 && (*early)[0].address == "127.0.0.9");
    CHECK(answer("getaddrinfo late.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
}

// The netid is netd's ParseUint into an unsigned (android-base/parseint.h): up to 2^32 - 1, hexadecimal after 0x, no
// sign (ADR-0054 step c2a, the review of step b).
TEST(the_netid_is_parsed_as_netds_parseuint) {
    const GuestSystem system(android_hosts);
    for (const char *netid : {"2147483648", "4294967295", "0x10", "0X1F", "+5"}) {
        const auto entries = entries_of(answer(std::string("getaddrinfo localhost ^ 0 0 1 0 ") + netid));
        CHECK(entries && entries->size() == 1);
        CHECK(host_of(answer(std::string("gethostbyname ") + netid + " localhost 2")).has_value());
        CHECK(host_of(answer(std::string("gethostbyaddr 127.0.0.1 4 2 ") + netid)).has_value());
    }
    for (const char *netid : {"4294967296", "-1", "-0", "1x", "0x", "\t"}) {
        CHECK(answer(std::string("getaddrinfo localhost ^ 0 0 1 0 ") + netid).starts_with("501 "));
        CHECK(answer(std::string("gethostbyname ") + netid + " localhost 2").starts_with("501 "));
        CHECK(answer(std::string("gethostbyaddr 127.0.0.1 4 2 ") + netid).starts_with("501 "));
    }
}

// Without INTERNET (ADR-0054 step c2a, a named departure): the proxy answers from the hosts file, and every other name
// and address as Android's DnsProxyListener answers a uid whose networking is blocked, EAI_FAIL and an empty failure,
// without asking the host's resolver. Android's own proxy checks no INTERNET permission; bionic turns numbers into
// addresses itself before it asks the proxy.
TEST(without_internet_the_proxy_answers_only_the_hosts_file) {
    // localhost is not in this file; the host's resolver answers it while INTERNET is granted.
    const GuestSystem system("10.9.8.7 filed.invalid\n");
    apkrt_set_internet(0);
    const auto entries = entries_of(answer("getaddrinfo filed.invalid ^ 0 0 1 0 0"));
    CHECK(entries && entries->size() == 1 && (*entries)[0].address == "10.9.8.7");
    constexpr int eai_fail = 4;  // bionic's
    CHECK(answer("getaddrinfo localhost ^ 0 0 1 0 0") == failure(int_bytes(eai_fail)));
    CHECK(answer("gethostbyname 0 localhost 2") == failure({}));
    CHECK(answer("gethostbyaddr 127.0.0.1 4 2 0") == failure({}));
    const auto file = host_of(answer("gethostbyaddr 10.9.8.7 4 2 0"));
    CHECK(file && file->name == "filed.invalid");
    apkrt_set_internet(1);
    CHECK(entries_of(answer("getaddrinfo localhost ^ 0 0 1 0 0")).has_value());
    CHECK(host_of(answer("gethostbyaddr 127.0.0.1 4 2 0")).has_value());
}

TEST(a_name_nobody_resolves_is_getaddrinfos_failure) {
    const GuestSystem system(android_hosts);
    CHECK(answer("getaddrinfo nothing.invalid ^ 0 0 1 0 0") == failure(int_bytes(eai_nodata)));
}

TEST(gethostbyname_answers_from_the_hosts_file_numbers_and_the_resolver) {
    const GuestSystem system(android_hosts);
    auto host = host_of(answer("gethostbyname 0 localhost 2"));
    CHECK(host && host->name == "localhost" && host->aliases.empty() && host->type == linux_inet && host->length == 4);
    // Each address goes in 16 bytes, an IPv4 one too (DnsProxyListener's sendhostent).
    CHECK(host && host->addresses == std::vector<std::string>{"127.0.0.1"} &&
          host->sizes == std::vector<std::size_t>{16});
    host = host_of(answer("gethostbyname 0 127.0.0.1 2"));
    CHECK(host && host->name == "127.0.0.1" && host->addresses == std::vector<std::string>{"127.0.0.1"});
    // Digits and dots are a number to inet_pton, which takes no short form, and never a name to look up.
    CHECK(answer("gethostbyname 0 127.1 2") == failure(""));
    host = host_of(answer("gethostbyname 0 ip6-localhost 10"));
    CHECK(host && host->name == "ip6-localhost" && host->type == linux_inet6 && host->length == 16 &&
          host->addresses == std::vector<std::string>{"::1"});
    CHECK(answer("gethostbyname 0 nothing.invalid 2") == failure(""));
}

TEST(gethostbyaddr_answers_from_the_hosts_file) {
    const GuestSystem system("127.0.0.1 localhost loopback\n::1 ip6-localhost\n");
    auto host = host_of(answer("gethostbyaddr 127.0.0.1 4 2 0"));
    CHECK(host && host->name == "localhost" && host->aliases == std::vector<std::string>{"loopback"} &&
          host->type == linux_inet && host->addresses == std::vector<std::string>{"127.0.0.1"});
    host = host_of(answer("gethostbyaddr ::1 16 10 0"));
    CHECK(host && host->name == "ip6-localhost" && host->type == linux_inet6);
    // An IPv4-mapped address is the IPv4 one.
    host = host_of(answer("gethostbyaddr ::ffff:127.0.0.1 16 10 0"));
    CHECK(host && host->name == "localhost" && host->type == linux_inet && host->length == 4);
    CHECK(answer("gethostbyaddr fe80::1 16 10 0") == failure(""));  // link-local: never asked
}

TEST(what_is_no_command_gets_a_code_of_500_or_501) {
    const GuestSystem system(android_hosts);
    CHECK(answer("getaddrinfo localhost").starts_with("501 "));
    CHECK(answer("gethostbyname localhost").starts_with("501 "));
    CHECK(answer("gethostbyname x localhost 2").starts_with("501 "));
    CHECK(answer("gethostbyaddr nonsense 4 2 0").starts_with("400 "));
    CHECK(answer("resnsend 0 0 AAAA").starts_with("500 "));
    CHECK(answer("").starts_with("500 "));
    const auto reply = answer("bogus");
    CHECK(!reply.empty() && reply.back() == '\0');
}

TEST(the_proxy_outlives_a_client_that_goes_before_its_reply) {
    const GuestSystem system(android_hosts);
    char pattern[] = "/tmp/apkrt-dns-socket-XXXXXX";
    const std::string folder = ::mkdtemp(pattern);
    sockaddr_un name{};
    name.sun_family = AF_UNIX;
    std::snprintf(name.sun_path, sizeof name.sun_path, "%s/p", folder.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(::bind(listener, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0 && ::listen(listener, 8) == 0);
    apkrt::dns_proxy::serve(listener);
    const auto ask = [&](bool wait) {
        const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(::connect(client, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0);
        const std::string command("getaddrinfo localhost ^ 0 0 1 0 0", 34);
        CHECK(::write(client, command.data(), command.size()) == static_cast<ssize_t>(command.size()));
        std::string reply;
        if (wait) {
            char buffer[512];
            for (ssize_t got; (got = ::read(client, buffer, sizeof buffer)) > 0;) {
                reply.append(buffer, static_cast<std::size_t>(got));
                if (entries_of(reply)) break;
            }
        }
        ::close(client);
        return reply;
    };
    static_cast<void>(ask(false));  // gone before the reply
    const auto entries = entries_of(ask(true));
    CHECK(entries && entries->size() == 1 && (*entries)[0].address == "127.0.0.1");
    // Two commands in one write, as FrameworkListener takes them: two replies, in order.
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(::connect(client, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0);
    const std::string commands("gethostbyname 0 localhost 2\0gethostbyname 0 ip6-localhost 10", 61);
    CHECK(::write(client, commands.data(), commands.size()) == static_cast<ssize_t>(commands.size()));
    const auto first = answer("gethostbyname 0 localhost 2"), second = answer("gethostbyname 0 ip6-localhost 10");
    std::string replies;
    char buffer[512];
    while (replies.size() < first.size() + second.size()) {
        const ssize_t got = ::read(client, buffer, sizeof buffer);
        if (got <= 0) break;
        replies.append(buffer, static_cast<std::size_t>(got));
    }
    CHECK(replies == first + second);
    ::close(client);
    // And one after the other on one connection, each after its reply.
    const int again = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const int one = 1;
    ::setsockopt(again, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    CHECK(::connect(again, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0);
    for (const std::string command : {"gethostbyname 0 localhost 2", "gethostbyname 0 ip6-localhost 10"}) {
        const std::string sent = command + '\0';
        CHECK(::write(again, sent.data(), sent.size()) == static_cast<ssize_t>(sent.size()));
        const auto expected = answer(command);
        std::string reply;
        for (ssize_t got; reply.size() < expected.size() && (got = ::read(again, buffer, sizeof buffer)) > 0;) {
            reply.append(buffer, static_cast<std::size_t>(got));
        }
        CHECK(reply == expected);
    }
    ::close(again);
    static_cast<void>(std::system(("rm -rf '" + folder + "'").c_str()));
}

/// A listening AF_UNIX socket in a folder of its own, and clients for it.
struct Listener {
    std::string folder;
    sockaddr_un name{};
    int fd = -1;

    Listener() {
        char pattern[] = "/tmp/apkrt-dns-listen-XXXXXX";
        folder = ::mkdtemp(pattern);
        name.sun_family = AF_UNIX;
        std::snprintf(name.sun_path, sizeof name.sun_path, "%s/p", folder.c_str());
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(::bind(fd, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0 && ::listen(fd, 8) == 0);
    }
    ~Listener() { static_cast<void>(std::system(("rm -rf '" + folder + "'").c_str())); }

    /// A connected client without SIGPIPE.
    int client() const {
        const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        CHECK(::connect(socket, reinterpret_cast<const sockaddr *>(&name), sizeof name) == 0);
        return socket;
    }
};

/// What `client` receives until it has `size` bytes, the peer closes, or `seconds` pass without a byte.
std::string receive(int client, std::size_t size, double seconds = 3) {
    std::string received;
    char buffer[512];
    while (received.size() < size) {
        pollfd ready{client, POLLIN, 0};
        if (::poll(&ready, 1, static_cast<int>(seconds * 1000)) <= 0) break;
        const ssize_t got = ::read(client, buffer, sizeof buffer);
        if (got <= 0) break;
        received.append(buffer, static_cast<std::size_t>(got));
    }
    return received;
}

/// Whether the peer of `client` closed it within `seconds`: a read that ends without data.
bool closed_by_peer(int client, double seconds = 3) {
    pollfd ready{client, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>(seconds * 1000)) <= 0) return false;
    char byte;
    return ::read(client, &byte, 1) == 0;
}

TEST(the_proxy_accepts_again_after_running_out_of_descriptors) {
    // EMFILE at accept is the process's limit for a while, not the listener's end (the review of b).
    const GuestSystem system(android_hosts);
    const Listener listener;
    std::atomic<bool> closed{false};
    CHECK(apkrt::dns_proxy::serve(listener.fd, [&] { closed = true; }));
    rlimit before{};
    CHECK(::getrlimit(RLIMIT_NOFILE, &before) == 0);
    const int lowest = ::dup(0);  // the lowest free descriptor
    ::close(lowest);
    rlimit tight = before;
    tight.rlim_cur = static_cast<rlim_t>(lowest) + 1;  // room for the client's socket alone
    CHECK(::setrlimit(RLIMIT_NOFILE, &tight) == 0);
    // This client's connection is lost: XNU's accept drops the connection it cannot give a descriptor ("that just
    // causes the client to spin"), where Linux leaves it queued.
    const int lost = listener.client();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // accept answers EMFILE meanwhile
    CHECK(::setrlimit(RLIMIT_NOFILE, &before) == 0);
    const int client = listener.client();  // the listener still serves
    const std::string command("gethostbyname 0 localhost 2", 28);  // with its NUL
    CHECK(::write(client, command.data(), command.size()) == static_cast<ssize_t>(command.size()));
    const auto expected = answer("gethostbyname 0 localhost 2");
    CHECK(receive(client, expected.size()) == expected);
    CHECK(!closed);
    ::close(client);
    ::close(lost);
}

TEST(the_proxy_closes_connections_beyond_its_limit_and_idle_ones) {
    const GuestSystem system(android_hosts);
    const Listener listener;
    CHECK(apkrt::dns_proxy::serve(listener.fd, {}, {.connections = 2, .idle = std::chrono::milliseconds(300)}));
    const int first = listener.client(), second = listener.client();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int third = listener.client();
    CHECK(closed_by_peer(third, 0.25));   // beyond the limit: closed at once
    CHECK(closed_by_peer(first) && closed_by_peer(second));  // idle: closed after 300 ms
    const int fourth = listener.client();  // the count went down again
    const std::string command("gethostbyname 0 localhost 2", 28);
    CHECK(::write(fourth, command.data(), command.size()) == static_cast<ssize_t>(command.size()));
    const auto expected = answer("gethostbyname 0 localhost 2");
    CHECK(receive(fourth, expected.size()) == expected);
    for (const int client : {first, second, third, fourth}) ::close(client);
}

TEST(a_command_too_large_gets_one_answer) {
    // FrameworkListener answers "500 Command too large for buffer" and skips to the next NUL, so that the rest of the
    // command is not taken for one of its own.
    const GuestSystem system(android_hosts);
    const Listener listener;
    CHECK(apkrt::dns_proxy::serve(listener.fd));
    const int client = listener.client();
    std::string sent(5000, 'x');
    sent += std::string("tail\0gethostbyname 0 localhost 2", 33);
    CHECK(::write(client, sent.data(), sent.size()) == static_cast<ssize_t>(sent.size()));
    const auto expected = std::string("500 Command too large for buffer", 33) + answer("gethostbyname 0 localhost 2");
    CHECK(receive(client, expected.size() + 1, 1) == expected);  // and nothing after it
    ::close(client);
}

TEST(serve_reports_a_listener_that_is_gone) {
    // accept's EINVAL, here for a socket that does not listen: the thread closes it and says so, for the runtime to
    // make another listener.
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    std::atomic<bool> closed{false};
    CHECK(apkrt::dns_proxy::serve(socket, [&] { closed = true; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!closed && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(closed);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
