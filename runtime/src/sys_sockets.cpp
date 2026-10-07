// Sockets of the Linux syscall layer (ADR-0054 step a): socket, socketpair, bind, listen, accept, accept4, connect,
// getsockname, getpeername, sendto, recvfrom, setsockopt, getsockopt, shutdown, sendmsg, recvmsg, sendmmsg and recvmmsg
// on Darwin's sockets. Linux's families, names, flags, options, ancillary data and answers are translated both ways,
// as fixtures/suite/socket-test finds them on Android 15. AF_UNIX's names in the file system reach their folder
// through a per-thread working directory, since Darwin's sun_path holds 104 bytes and an app's folders lie deep in
// its container; its abstract names, which Darwin lacks, are sockets in a folder of the runtime's own.
#define __APPLE_USE_RFC_3542 1  // RFC 3542's IPV6_PKTINFO and its relatives

#include "sockets.h"

#include "descriptors.h"
#include "identity.h"
#include "signals.h"
#include "syscalls.h"

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// libSystem's per-thread working directory, exported since macOS 10.12 and iOS 10 (fts's FTS_THREAD_FCHDIR uses it)
// but not in the SDK's headers: -1 goes back to the process's.
extern "C" int pthread_fchdir_np(int fd);

namespace apkrt::sockets {
namespace {

namespace linux_errno = sys::linux_errno;

// Linux's numbers (include/linux/socket.h, include/uapi/asm-generic/socket.h, include/uapi/linux/in.h, in6.h, tcp.h).
namespace linux_net {
constexpr int af_unspec = 0, af_unix = 1, af_inet = 2, af_inet6 = 10, af_netlink = 16, af_packet = 17, af_max = 46;
constexpr int sock_stream = 1, sock_dgram = 2, sock_raw = 3, sock_seqpacket = 5, sock_max = 11, type_mask = 0xf;
constexpr int sock_nonblock = 04000, sock_cloexec = 02000000;
constexpr int sol_socket = 1, ipproto_ip = 0, ipproto_icmp = 1, ipproto_tcp = 6, ipproto_udp = 17, ipproto_ipv6 = 41,
              ipproto_icmpv6 = 58;
constexpr int so_debug = 1, so_reuseaddr = 2, so_type = 3, so_error = 4, so_dontroute = 5, so_broadcast = 6,
              so_sndbuf = 7, so_rcvbuf = 8, so_keepalive = 9, so_oobinline = 10, so_priority = 12, so_linger = 13,
              so_reuseport = 15, so_passcred = 16, so_peercred = 17, so_rcvlowat = 18, so_sndlowat = 19,
              so_rcvtimeo_old = 20, so_sndtimeo_old = 21, so_bindtodevice = 25, so_timestamp_old = 29,
              so_acceptconn = 30, so_sndbufforce = 32, so_rcvbufforce = 33, so_mark = 36, so_protocol = 38,
              so_domain = 39, so_cookie = 57, so_rcvtimeo_new = 66, so_sndtimeo_new = 67;
constexpr int scm_rights = 1, scm_credentials = 2;
constexpr int ip_tos = 1, ip_ttl = 2, ip_pktinfo = 8, ip_mtu_discover = 10, ip_recverr = 11, ip_recvttl = 12,
              ip_recvtos = 13, ip_freebind = 15, ip_transparent = 19, ip_bind_address_no_port = 24,
              ip_multicast_if = 32, ip_multicast_ttl = 33, ip_multicast_loop = 34, ip_add_membership = 35,
              ip_drop_membership = 36, ip_multicast_all = 49;
constexpr int ipv6_unicast_hops = 16, ipv6_multicast_if = 17, ipv6_multicast_hops = 18, ipv6_multicast_loop = 19,
              ipv6_join_group = 20, ipv6_leave_group = 21, ipv6_mtu_discover = 23, ipv6_recverr = 25, ipv6_v6only = 26,
              ipv6_recvpktinfo = 49, ipv6_pktinfo = 50, ipv6_recvhoplimit = 51, ipv6_hoplimit = 52,
              ipv6_recvtclass = 66, ipv6_tclass = 67;
constexpr int tcp_nodelay = 1, tcp_maxseg = 2, tcp_cork = 3, tcp_keepidle = 4, tcp_keepintvl = 5, tcp_keepcnt = 6,
              tcp_syncnt = 7, tcp_linger2 = 8, tcp_defer_accept = 9, tcp_window_clamp = 10, tcp_info = 11,
              tcp_quickack = 12, tcp_user_timeout = 18, tcp_fastopen = 23, tcp_notsent_lowat = 25;
constexpr int msg_oob = 1, msg_peek = 2, msg_dontroute = 4, msg_ctrunc = 8, msg_trunc = 0x20, msg_dontwait = 0x40,
              msg_eor = 0x80, msg_waitall = 0x100, msg_errqueue = 0x2000, msg_nosignal = 0x4000,
              msg_waitforone = 0x10000, msg_cmsg_cloexec = 0x40000000;
}  // namespace linux_net

using namespace linux_net;

// Linux's structures on arm64.
struct LinuxMsghdr {
    std::uint64_t name;
    std::uint32_t name_length, pad;
    std::uint64_t vectors, vector_count, control, control_length;
    std::int32_t flags;
    std::uint32_t pad2;
};
static_assert(sizeof(LinuxMsghdr) == 56);

struct LinuxMmsghdr {
    LinuxMsghdr header;
    std::uint32_t length, pad;
};
static_assert(sizeof(LinuxMmsghdr) == 64);

struct LinuxCmsghdr {
    std::uint64_t length;
    std::int32_t level, type;
};
static_assert(sizeof(LinuxCmsghdr) == 16);

struct LinuxUcred {
    std::int32_t pid;
    std::uint32_t uid, gid;
};

struct LinuxTimeval {
    std::int64_t seconds, microseconds;
};

constexpr std::size_t linux_cmsg_align(std::size_t length) {
    return (length + 7) & ~std::size_t{7};
}
constexpr std::size_t linux_cmsg_length(std::size_t data) {
    return sizeof(LinuxCmsghdr) + data;
}
constexpr std::size_t linux_cmsg_space(std::size_t data) {
    return sizeof(LinuxCmsghdr) + linux_cmsg_align(data);
}

/// An unnamed AF_UNIX socket's name, as getsockname, getpeername and accept give it: the family alone.
const std::string unnamed("\1\0", 2);

/// Linux's sockaddr_un holds 110 bytes; Linux's sockaddr_storage 128.
constexpr std::uint64_t linux_unix_size = 110, linux_storage_size = 128;

std::uint16_t read_u16(const unsigned char *bytes) {
    std::uint16_t value;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

// --- The registry of AF_UNIX names ------------------------------------------------------------------------------

/// The host names Darwin keeps for bound AF_UNIX sockets, which getpeername, accept and recvfrom give back, and the
/// guest names they stand for; the abstract names and their sockets in the runtime's folder.
struct UnixNames {
    std::mutex mutex;
    std::map<std::string, std::string> guest_of_host;     // a host name as Darwin keeps it → the guest's name
    std::map<std::string, std::string> host_of_abstract;  // an abstract guest name → its host name in `folder`
    int folder = -1;
    std::uint64_t made = 0;
};

UnixNames &unix_names() {
    static auto *names = new UnixNames;  // as long as the process
    return *names;
}

/// Empties the folder `folder` of its files.
void empty(int folder) {
    const int copy = ::dup(folder);
    DIR *directory = copy >= 0 ? ::fdopendir(copy) : nullptr;
    if (directory == nullptr) {
        if (copy >= 0) ::close(copy);
        return;
    }
    std::vector<std::string> entries;
    while (const dirent *entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) entries.emplace_back(entry->d_name);
    }
    ::closedir(directory);
    for (const auto &entry : entries) ::unlinkat(folder, entry.c_str(), 0);
}

constexpr std::string_view folder_prefix = "apkrt-unix-";

/// The runtime's folder of abstract names, in the user's temporary folder, made at its first use, when the folders
/// of processes that are gone go; -1 when it cannot be made. Under `names.mutex`.
int abstract_folder(UnixNames &names) {
    if (names.folder >= 0) return names.folder;
    char temporary[MAXPATHLEN];
    if (::confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof temporary) == 0) return -1;
    const int parent = ::open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return -1;
    if (const int copy = ::dup(parent); copy >= 0) {
        if (DIR *directory = ::fdopendir(copy)) {
            std::vector<std::string> stale;
            while (const dirent *entry = ::readdir(directory)) {
                const std::string_view name = entry->d_name;
                if (!name.starts_with(folder_prefix)) continue;
                const long pid = std::strtol(entry->d_name + folder_prefix.size(), nullptr, 10);
                if (pid > 0 && pid != ::getpid() && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
                    stale.emplace_back(name);
                }
            }
            ::closedir(directory);
            for (const auto &name : stale) {
                const int old = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (old >= 0) {
                    empty(old);
                    ::close(old);
                }
                ::unlinkat(parent, name.c_str(), AT_REMOVEDIR);
            }
        } else {
            ::close(copy);
        }
    }
    const std::string own = std::string(folder_prefix) + std::to_string(::getpid());
    ::mkdirat(parent, own.c_str(), 0700);
    names.folder = ::openat(parent, own.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ::close(parent);
    if (names.folder >= 0) empty(names.folder);  // left by a process that had this pid before
    return names.folder;
}

/// The folder that the calling thread's working directory is at during in_folder; -1 for the process's.
thread_local int thread_folder = -1;

/// Makes `call` (Linux's answer) with the calling thread's working directory at `folder`, unless that is AT_FDCWD,
/// and puts back the one before, also for a call within another's.
template <typename Call>
long in_folder(int folder, Call call) {
    if (folder == AT_FDCWD) return call();
    const int outer = thread_folder;
    if (::pthread_fchdir_np(folder) != 0) return -sys::to_linux_errno(errno);
    thread_folder = folder;
    const long result = call();
    ::pthread_fchdir_np(outer);
    thread_folder = outer;
    return result;
}

/// A name for one of the runtime's host sockets, with `prefix`: unguessable, so that no link that a guest plants can
/// stand in its place when the runtime binds under it.
std::string unguessable_name(std::string_view prefix) {
    std::uint64_t bits[2];
    ::arc4random_buf(bits, sizeof bits);
    char digits[33];
    std::snprintf(digits, sizeof digits, "%016llx%016llx", static_cast<unsigned long long>(bits[0]),
                  static_cast<unsigned long long>(bits[1]));
    return std::string(prefix) + digits;
}

/// A Darwin sockaddr_un of `name`, a path relative to the working directory or a short absolute one; 0 when it does
/// not fit.
socklen_t host_unix_name(std::string_view name, sockaddr_un &out) {
    out = {};
    if (name.size() >= sizeof out.sun_path) return 0;
    out.sun_family = AF_UNIX;
    std::memcpy(out.sun_path, name.data(), name.size());
    out.sun_len = static_cast<unsigned char>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
    return out.sun_len;
}

// --- Descriptors in flight ---------------------------------------------------------------------------------------

/// The descriptions this process sent with SCM_RIGHTS and has not received yet. Each goes as a token, one end of a
/// socketpair of its own, which no other descriptor shares an inode with: a received token stands for exactly the
/// description sent, with its guest O_NONBLOCK and its state, as Linux passes the open file itself. The runtime
/// keeps the token's other end, which sees the token's close when a message goes without being received.
struct InFlight {
    descriptors::Handle handle;
    int kept = -1;
};

struct Flight {
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, InFlight> tokens;  // by the token's device and inode
};

/// Descriptors in flight at most: a send beyond is ETOOMANYREFS, as Linux's beyond RLIMIT_NOFILE
/// (too_many_unix_fds), which is higher; each costs the runtime a host descriptor.
constexpr std::size_t most_in_flight = 4096;

Flight &flight() {
    static auto *instance = new Flight;
    return *instance;
}

std::pair<std::uint64_t, std::uint64_t> token_key(int host) {
    struct stat status{};
    if (::fstat(host, &status) != 0) return {0, 0};
    return {static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)};
}

/// Forgets the tokens of messages that went without being received: their kept end sees the end. Under the mutex.
void sweep(Flight &in_flight) {
    for (auto entry = in_flight.tokens.begin(); entry != in_flight.tokens.end();) {
        pollfd kept{entry->second.kept, POLLIN, 0};
        if (::poll(&kept, 1, 0) == 1) {
            ::close(entry->second.kept);
            entry = in_flight.tokens.erase(entry);
        } else {
            ++entry;
        }
    }
}

/// The description that the received token `host` stands for, or none; its kept end goes.
descriptors::Handle take_in_flight(int host) {
    auto &in_flight = flight();
    const std::lock_guard lock(in_flight.mutex);
    const auto found = in_flight.tokens.find(token_key(host));
    if (found == in_flight.tokens.end()) return {};
    auto handle = std::move(found->second.handle);
    ::close(found->second.kept);
    in_flight.tokens.erase(found);
    return handle;
}

// --- Host sockets ------------------------------------------------------------------------------------------------

/// Makes a new host socket the runtime's way: close-on-exec, non-blocking and without SIGPIPE; closes it on failure.
long prepare(int host) {
    const int on = 1;
    if (::fcntl(host, F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(host, F_SETFL, ::fcntl(host, F_GETFL) | O_NONBLOCK) != 0 ||
        ::setsockopt(host, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0) {
        const int error = errno;
        ::close(host);
        return -sys::to_linux_errno(error);
    }
    return 0;
}

/// Android 15's buffers [verified: socket-test, the emulator's /proc/sys/net]: wmem_default and rmem_default, which
/// an AF_UNIX, UDP or ping socket has before SO_SNDBUF and SO_RCVBUF set them; wmem_max and rmem_max, which cap what
/// they set before Linux doubles it; and the middle values of tcp_wmem and tcp_rmem, which a TCP socket starts with.
constexpr int linux_buffer = 229'376, linux_buffer_max = 262'144, tcp_send_buffer = 2'097'152,
              tcp_receive_buffer = 6'291'456;

/// Makes the host's send buffer of an AF_UNIX datagram socket whose Linux sk_sndbuf is `linux_size` refuse what
/// unix_dgram_sendmsg refuses: Darwin refuses a datagram longer than its send buffer with EMSGSIZE, Linux one longer
/// than sk_sndbuf less 32 bytes.
void limit_datagrams(int host, int linux_size) {
    const int most = linux_size - 32;
    ::setsockopt(host, SOL_SOCKET, SO_SNDBUF, &most, sizeof most);
}

/// Linux's family of a Darwin one.
int linux_family(int host_family) {
    switch (host_family) {
        case AF_UNIX: return af_unix;
        case AF_INET: return af_inet;
        case AF_INET6: return af_inet6;
        default: return af_unspec;
    }
}

/// The guest's name of the host's `address`, for `family`: Linux's bytes, or empty for an unnamed AF_UNIX socket.
std::string guest_name_of(const sockaddr_storage &address, socklen_t length) {
    switch (address.ss_family) {
        case AF_INET: {
            const auto &in = reinterpret_cast<const sockaddr_in &>(address);
            unsigned char bytes[16] = {};
            const std::uint16_t family = af_inet;
            std::memcpy(bytes, &family, 2);
            std::memcpy(bytes + 2, &in.sin_port, 2);
            std::memcpy(bytes + 4, &in.sin_addr, 4);
            return std::string(reinterpret_cast<const char *>(bytes), sizeof bytes);
        }
        case AF_INET6: {
            const auto &in6 = reinterpret_cast<const sockaddr_in6 &>(address);
            unsigned char bytes[28] = {};
            const std::uint16_t family = af_inet6;
            std::memcpy(bytes, &family, 2);
            std::memcpy(bytes + 2, &in6.sin6_port, 2);
            std::memcpy(bytes + 4, &in6.sin6_flowinfo, 4);
            std::memcpy(bytes + 8, &in6.sin6_addr, 16);
            std::memcpy(bytes + 24, &in6.sin6_scope_id, 4);
            return std::string(reinterpret_cast<const char *>(bytes), sizeof bytes);
        }
        case AF_UNIX: {
            const auto &un = reinterpret_cast<const sockaddr_un &>(address);
            if (length <= offsetof(sockaddr_un, sun_path) || un.sun_path[0] == '\0') return {};
            const std::string host(un.sun_path, ::strnlen(un.sun_path, length - offsetof(sockaddr_un, sun_path)));
            auto &names = unix_names();
            const std::lock_guard lock(names.mutex);
            const auto found = names.guest_of_host.find(host);
            return found != names.guest_of_host.end() ? found->second : std::string();
        }
        default: return {};
    }
}

/// move_addr_to_user: `name` cut to the room the guest gave at `length_address`, which then holds its whole length.
long put_name(std::string_view name, std::uint64_t address, std::uint64_t length_address) {
    if (length_address == 0) return -linux_errno::efault;
    std::int32_t room;
    std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
    if (room < 0) return -linux_errno::einval;
    const std::size_t copied = std::min(static_cast<std::size_t>(room), name.size());
    if (copied > 0) {
        if (address == 0) return -linux_errno::efault;
        std::memcpy(sys::as_pointer<void>(address), name.data(), copied);
    }
    const auto whole = static_cast<std::int32_t>(name.size());
    std::memcpy(sys::as_pointer<void>(length_address), &whole, sizeof whole);
    return 0;
}

// --- Names from the guest ----------------------------------------------------------------------------------------

enum class Use { bind, connect, send };

/// A guest name for a host call: Darwin's sockaddr, and for AF_UNIX the folder the call starts in.
struct HostName {
    sockaddr_storage storage{};
    socklen_t length = 0;
    Place place;             // AF_UNIX in the file system: its folder and last component
    int folder = AT_FDCWD;   // the call's working directory: the place's, or the folder of abstract names
    std::string guest;       // AF_UNIX: the guest's name, as Linux keeps it
    bool unspecified = false;  // AF_UNSPEC to connect: the end of a datagram socket's association
    bool autobind = false;     // bind of the family alone

    const sockaddr *address() const { return reinterpret_cast<const sockaddr *>(&storage); }
};

/// Whether the host's AF_INET6 socket takes IPv6 alone.
bool v6only(int host) {
    int value = 0;
    socklen_t length = sizeof value;
    return ::getsockopt(host, IPPROTO_IPV6, IPV6_V6ONLY, &value, &length) == 0 && value != 0;
}

/// An AF_INET or AF_INET6 name, with inet_bind's, tcp_v4_connect's, inet6_bind's, ip6_datagram_connect's,
/// udp_sendmsg's and udpv6_sendmsg's checks.
long inet_name(Socket &socket, const unsigned char *bytes, std::uint64_t length, int family, Use use, HostName &out) {
    const bool stream = socket.type == sock_stream;
    if (family == af_unspec && use == Use::connect) {
        out.unspecified = true;
        return 0;
    }
    const auto ipv4 = [&](std::uint16_t port_bytes, const unsigned char *address) {
        auto &in = reinterpret_cast<sockaddr_in &>(out.storage);
        in.sin_len = sizeof in;
        in.sin_family = AF_INET;
        in.sin_port = port_bytes;
        std::memcpy(&in.sin_addr, address, 4);
        out.length = sizeof in;
    };
    const auto privileged = [](std::uint16_t port_bytes) {
        const std::uint16_t port = ntohs(port_bytes);
        return port != 0 && port < 1024;  // CAP_NET_BIND_SERVICE, which an app has not [inferred: ip_unprivileged_port_start 1024]
    };
    if (socket.family == af_inet) {
        if (length < 16) return -linux_errno::einval;
        std::uint32_t address;
        std::memcpy(&address, bytes + 4, 4);
        const bool any_unspec = family == af_unspec && (use == Use::send || (use == Use::bind && address == 0));
        if (family != af_inet && !any_unspec) return -linux_errno::eafnosupport;
        const std::uint16_t port = read_u16(bytes + 2);
        if (use == Use::bind && privileged(port)) return -linux_errno::eacces;
        ipv4(port, bytes + 4);
        return 0;
    }
    // AF_INET6.
    if (family == af_inet && !(use == Use::bind || (use == Use::connect && stream))) {
        if (length < 16) return -linux_errno::einval;
        if (v6only(socket.host())) return use == Use::connect ? -linux_errno::eafnosupport : -linux_errno::enetunreach;
        auto &in6 = reinterpret_cast<sockaddr_in6 &>(out.storage);
        in6.sin6_len = sizeof in6;
        in6.sin6_family = AF_INET6;
        in6.sin6_port = read_u16(bytes + 2);
        in6.sin6_addr.s6_addr[10] = 0xff;
        in6.sin6_addr.s6_addr[11] = 0xff;
        std::memcpy(&in6.sin6_addr.s6_addr[12], bytes + 4, 4);
        out.length = sizeof in6;
        return 0;
    }
    if (family != af_inet6) {
        if (use == Use::send) return -linux_errno::einval;
        return length < 24 ? -linux_errno::einval : -linux_errno::eafnosupport;
    }
    if (length < 24) return -linux_errno::einval;  // SIN6_LEN_RFC2133
    auto &in6 = reinterpret_cast<sockaddr_in6 &>(out.storage);
    in6.sin6_len = sizeof in6;
    in6.sin6_family = AF_INET6;
    in6.sin6_port = read_u16(bytes + 2);
    std::memcpy(&in6.sin6_flowinfo, bytes + 4, 4);
    std::memcpy(&in6.sin6_addr, bytes + 8, 16);
    if (length >= 28) std::memcpy(&in6.sin6_scope_id, bytes + 24, 4);
    if (use == Use::bind && privileged(in6.sin6_port)) return -linux_errno::eacces;
    out.length = sizeof in6;
    return 0;
}

/// An AF_UNIX name, with unix_validate_addr's checks: in the file system, its place; abstract, its socket in the
/// runtime's folder; the family alone, autobind for bind.
long unix_name(const unsigned char *bytes, std::uint64_t length, Use use, HostName &out) {
    if (length > linux_unix_size) return -linux_errno::einval;
    if (length < 2 || read_u16(bytes) != af_unix) return -linux_errno::einval;
    if (length == 2) {
        if (use != Use::bind) return -linux_errno::einval;
        out.autobind = true;
        return 0;
    }
    const auto *path = reinterpret_cast<const char *>(bytes + 2);
    const std::size_t size = static_cast<std::size_t>(length) - 2;
    sockaddr_un host{};
    if (path[0] == '\0') {
        out.guest.assign(reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(length));
        if (use == Use::bind) return 0;  // bind_unix makes its socket
        auto &names = unix_names();
        const std::lock_guard lock(names.mutex);
        const auto found = names.host_of_abstract.find(out.guest);
        if (found == names.host_of_abstract.end()) return -linux_errno::econnrefused;
        out.folder = names.folder;
        out.length = host_unix_name(found->second, host);
    } else {
        const std::string guest_path(path, ::strnlen(path, size));
        out.guest = unnamed + guest_path;
        out.guest += '\0';
        out.place = place_of(guest_path, use == Use::bind);
        if (out.place.error != 0) return out.place.error;
        if (use == Use::bind) return 0;  // bind_unix binds a name of its own and moves it there
        out.folder = out.place.folder;
        out.length = host_unix_name(out.place.name, host);
    }
    if (out.length == 0) return -linux_errno::enametoolong;
    std::memcpy(&out.storage, &host, sizeof host);
    return 0;
}

/// The guest's name at `address`, `length` bytes, for `use` on `socket`; Linux's errno on failure.
long host_name_of(Socket &socket, std::uint64_t address, std::uint64_t length, Use use, HostName &out) {
    if (length > linux_storage_size) return -linux_errno::einval;
    if (length > 0 && address == 0) return -linux_errno::efault;
    unsigned char bytes[linux_storage_size] = {};
    if (length > 0) std::memcpy(bytes, sys::as_pointer<void>(address), static_cast<std::size_t>(length));
    if (socket.family == af_unix) return unix_name(bytes, length, use, out);
    const int family = length >= 2 ? read_u16(bytes) : -1;
    return inet_name(socket, bytes, length, family, use, out);
}

// --- Waits -------------------------------------------------------------------------------------------------------

/// Makes `call` (Linux's answer) until it answers anything but EAGAIN, and in between, unless the guest does not wait
/// (`dont_wait`), waits for the socket to be ready for `writing` or reading: until its timeout for that direction
/// (EAGAIN) or a signal (EINTR).
template <typename Call>
long wait_ready(Socket &socket, bool writing, bool dont_wait, Call call) {
    long result = 0;
    const auto done = [&] {
        do {
            result = call();
        } while (result == -linux_errno::eintr);  // a SIGURG in the host's call: again
        return result != -linux_errno::eagain || dont_wait;
    };
    auto &state = signals::current();
    sys::Watched watched{socket.host(), static_cast<std::int16_t>(writing ? EVFILT_WRITE : EVFILT_READ)};
    const auto end = sys::wait_for_descriptors(state, &watched, 1, done, deadline_of(socket, writing));
    if (end == sys::WaitEnd::signal) return -linux_errno::eintr;
    if (end == sys::WaitEnd::timeout) return -linux_errno::eagain;
    return result;
}

/// Whether a connect under way on `host` has ended, well or not.
bool connect_ended(int host) {
    pollfd watched{host, POLLOUT, 0};
    return ::poll(&watched, 1, 0) == 1 && (watched.revents & (POLLOUT | POLLHUP | POLLERR)) != 0;
}

/// Linux's states of a connect over Darwin's non-blocking one: EINPROGRESS for a guest that does not wait, EALREADY
/// while it lasts; 0 for the first connect after it ended, EISCONN for any after that; for a guest that waits, the
/// wait, with SO_SNDTIMEO's EINPROGRESS at its end.
template <typename Call>
long connect_wait(Socket &socket, Call call) {
    const int host = socket.host();
    bool connecting;
    {
        const std::lock_guard lock(socket.mutex);
        connecting = socket.connecting;
    }
    if (!connecting) {
        const long result = call();
        if (result == 0) {
            const std::lock_guard lock(socket.mutex);
            socket.connected = true;
            return 0;
        }
        if (result != -linux_errno::einprogress) return result;
        {
            const std::lock_guard lock(socket.mutex);
            socket.connecting = true;
        }
        if (socket.nonblocking()) return result;
    } else if (socket.nonblocking() && !connect_ended(host)) {
        return -linux_errno::ealready;
    }
    auto &state = signals::current();
    sys::Watched watched{host, EVFILT_WRITE};
    const auto end = sys::wait_for_descriptors(state, &watched, 1, [&] { return connect_ended(host); },
                                               deadline_of(socket, true));
    if (end == sys::WaitEnd::signal) return -linux_errno::eintr;  // still under way: the call again waits on
    if (end == sys::WaitEnd::timeout) return -linux_errno::einprogress;
    int error = 0;
    socklen_t size = sizeof error;
    ::getsockopt(host, SOL_SOCKET, SO_ERROR, &error, &size);
    const std::lock_guard lock(socket.mutex);
    socket.connecting = false;
    if (error != 0) return -sys::to_linux_errno(error);
    socket.connected = true;
    return 0;
}

// --- Ancillary data ----------------------------------------------------------------------------------------------

/// The guest's ancillary data of a sendmsg as the host's: SCM_RIGHTS with a token for each of the guest's descriptors
/// (whose descriptions are in flight from then on, until the message is out or fails), Linux's credentials checked
/// and dropped (the receiver makes its own), and the packet options renumbered.
struct HostControl {
    std::vector<unsigned char> bytes;
    std::vector<int> tokens;  // the ends that go
    bool delivered = false;   // the message is out, and holds the tokens

    HostControl() = default;
    HostControl(const HostControl &) = delete;
    HostControl &operator=(const HostControl &) = delete;
    ~HostControl() {
        if (!delivered && !tokens.empty()) {
            auto &in_flight = flight();
            const std::lock_guard lock(in_flight.mutex);
            for (const int token : tokens) {
                const auto found = in_flight.tokens.find(token_key(token));
                if (found == in_flight.tokens.end()) continue;
                ::close(found->second.kept);
                in_flight.tokens.erase(found);
            }
        }
        for (const int token : tokens) ::close(token);
    }

    void put(int level, int type, const void *data, std::size_t length) {
        const std::size_t at = bytes.size();
        bytes.resize(at + CMSG_SPACE(length));
        auto *header = reinterpret_cast<cmsghdr *>(bytes.data() + at);
        header->cmsg_len = static_cast<socklen_t>(CMSG_LEN(length));
        header->cmsg_level = level;
        header->cmsg_type = type;
        std::memcpy(CMSG_DATA(header), data, length);
    }
};

long host_control_of(const LinuxMsghdr &message, HostControl &out) {
    if (message.control == 0 || message.control_length < sizeof(LinuxCmsghdr)) return 0;
    const auto *base = sys::as_pointer<const unsigned char>(message.control);
    const std::uint64_t end = message.control_length;
    for (std::uint64_t at = 0; at + sizeof(LinuxCmsghdr) <= end;) {
        LinuxCmsghdr header;
        std::memcpy(&header, base + at, sizeof header);
        if (header.length < sizeof header || header.length > end - at) return -linux_errno::einval;
        const unsigned char *data = base + at + sizeof header;
        const std::size_t size = static_cast<std::size_t>(header.length - sizeof header);
        if (header.level == sol_socket && header.type == scm_rights) {
            const std::size_t count = size / sizeof(int);
            if (out.tokens.size() + count > 253) return -linux_errno::einval;  // SCM_MAX_FD
            std::vector<descriptors::Handle> handles;
            for (std::size_t index = 0; index < count; ++index) {
                int fd;
                std::memcpy(&fd, data + index * sizeof(int), sizeof fd);
                auto description = descriptors::get(fd);
                if (!description) return -linux_errno::ebadf;
                handles.push_back(std::move(description));
            }
            auto &in_flight = flight();
            const std::lock_guard lock(in_flight.mutex);
            if (in_flight.tokens.size() + count > most_in_flight) sweep(in_flight);
            if (in_flight.tokens.size() + count > most_in_flight) return -linux_errno::etoomanyrefs;
            std::vector<int> hosts;
            for (auto &handle : handles) {
                int token[2];
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, token) != 0) return -sys::to_linux_errno(errno);
                ::fcntl(token[0], F_SETFD, FD_CLOEXEC);
                ::fcntl(token[1], F_SETFD, FD_CLOEXEC);
                out.tokens.push_back(token[0]);
                in_flight.tokens[token_key(token[0])] = {std::move(handle), token[1]};
                hosts.push_back(token[0]);
            }
            out.put(SOL_SOCKET, SCM_RIGHTS, hosts.data(), hosts.size() * sizeof(int));
        } else if (header.level == sol_socket && header.type == scm_credentials) {
            LinuxUcred credentials{};
            if (size < sizeof credentials) return -linux_errno::einval;
            std::memcpy(&credentials, data, sizeof credentials);
            // scm_check_creds: only the process's own, without CAP_SETUID and its relatives.
            if (credentials.pid != ::getpid() || credentials.uid != app_uid || credentials.gid != app_uid) {
                return -linux_errno::eperm;
            }
        } else if (header.level == ipproto_ip && header.type == ip_pktinfo && size >= sizeof(in_pktinfo)) {
            out.put(IPPROTO_IP, IP_PKTINFO, data, sizeof(in_pktinfo));
        } else if (header.level == ipproto_ipv6 && header.type == ipv6_pktinfo && size >= sizeof(in6_pktinfo)) {
            out.put(IPPROTO_IPV6, IPV6_PKTINFO, data, sizeof(in6_pktinfo));
        } else if (header.level == ipproto_ipv6 && (header.type == ipv6_hoplimit || header.type == ipv6_tclass) &&
                   size >= sizeof(int)) {
            out.put(IPPROTO_IPV6, header.type == ipv6_hoplimit ? IPV6_HOPLIMIT : IPV6_TCLASS, data, sizeof(int));
        } else {
            return -linux_errno::einval;
        }
        at += linux_cmsg_align(static_cast<std::size_t>(header.length));
    }
    return 0;
}

/// The guest's ancillary data of a recvmsg, as Linux's put_cmsg and scm_detach_fds write it into the guest's buffer.
class GuestControl {
public:
    GuestControl(std::uint64_t control, std::uint64_t room, std::int32_t &flags)
        : base_(control == 0 ? nullptr : sys::as_pointer<unsigned char>(control)), room_(control == 0 ? 0 : room),
          flags_(flags) {}

    std::uint64_t used() const { return used_; }

    void put(int level, int type, const void *data, std::size_t length) {
        const std::uint64_t left = room_ - used_;
        if (left < sizeof(LinuxCmsghdr)) {
            flags_ |= msg_ctrunc;
            return;
        }
        std::uint64_t written = linux_cmsg_length(length);
        if (left < written) {
            flags_ |= msg_ctrunc;
            written = left;
        }
        const LinuxCmsghdr header{written, level, type};
        std::memcpy(base_ + used_, &header, sizeof header);
        std::memcpy(base_ + used_ + sizeof header, data, static_cast<std::size_t>(written - sizeof header));
        used_ += std::min<std::uint64_t>(linux_cmsg_space(length), left);
    }

    /// How many of `count` descriptors fit, as scm_detach_fds counts them; MSG_CTRUNC when not all.
    std::size_t room_for_descriptors(std::size_t count) {
        const std::uint64_t left = room_ - used_;
        const std::size_t most = left > sizeof(LinuxCmsghdr) ? static_cast<std::size_t>((left - sizeof(LinuxCmsghdr)) / sizeof(int)) : 0;
        const std::size_t fit = std::min(count, most);
        if (fit < count) flags_ |= msg_ctrunc;
        return fit;
    }

private:
    unsigned char *base_;
    std::uint64_t room_, used_ = 0;
    std::int32_t &flags_;
};

/// A received host descriptor as the guest's: the description in flight from this process that it is the token
/// of, or a new one.
long adopt_received(int host, bool close_on_exec) {
    if (auto handle = take_in_flight(host)) {
        ::close(host);  // the token
        return descriptors::add(std::move(handle), close_on_exec);
    }
    struct stat status{};
    if (::fstat(host, &status) == 0 && S_ISSOCK(status.st_mode)) {
        sockaddr_storage name{};
        socklen_t length = sizeof name;
        int type = SOCK_STREAM;
        socklen_t type_length = sizeof type;
        ::getsockname(host, reinterpret_cast<sockaddr *>(&name), &length);
        ::getsockopt(host, SOL_SOCKET, SO_TYPE, &type, &type_length);
        if (const long error = prepare(host)) return error;
        const int family = linux_family(name.ss_family);
        const int protocol = family == af_unix ? 0 : (type == SOCK_STREAM ? ipproto_tcp : ipproto_udp);
        return descriptors::add(std::make_shared<Socket>(host, false, family, type, protocol), close_on_exec);
    }
    return descriptors::add(host, close_on_exec);
}

/// The host's ancillary data of a recvmsg as the guest's. A peek passes no descriptors: Darwin gives their slots as
/// zeros then, where Linux gives copies (unix_peek_fds), and the receive after it gets them.
void guest_control_of(Socket &socket, msghdr &host, GuestControl &out, bool close_on_exec, bool peek) {
    if (socket.pass_credentials && socket.family == af_unix) {
        const LinuxUcred credentials{::getpid(), app_uid, app_uid};
        out.put(sol_socket, scm_credentials, &credentials, sizeof credentials);
    }
    if (host.msg_controllen == 0 || host.msg_control == nullptr) return;
    for (cmsghdr *header = CMSG_FIRSTHDR(&host); header != nullptr; header = CMSG_NXTHDR(&host, header)) {
        const unsigned char *data = CMSG_DATA(header);
        const std::size_t size = header->cmsg_len - static_cast<std::size_t>(data - reinterpret_cast<unsigned char *>(header));
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            if (peek) continue;
            std::vector<int> hosts(size / sizeof(int));
            std::memcpy(hosts.data(), data, hosts.size() * sizeof(int));
            const std::size_t fit = out.room_for_descriptors(hosts.size());
            std::vector<int> guests;
            for (std::size_t index = 0; index < hosts.size(); ++index) {
                if (index >= fit) {
                    // Dropped, as Linux drops what does not fit: its description too, if it was in flight.
                    static_cast<void>(take_in_flight(hosts[index]));
                    ::close(hosts[index]);
                    continue;
                }
                const long guest = adopt_received(hosts[index], close_on_exec);
                if (guest >= 0) guests.push_back(static_cast<int>(guest));
            }
            if (!guests.empty()) out.put(sol_socket, scm_rights, guests.data(), guests.size() * sizeof(int));
        } else if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_TIMESTAMP && size >= sizeof(timeval)) {
            timeval time;
            std::memcpy(&time, data, sizeof time);
            const LinuxTimeval guest{time.tv_sec, time.tv_usec};
            out.put(sol_socket, so_timestamp_old, &guest, sizeof guest);
        } else if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO && size >= sizeof(in_pktinfo)) {
            out.put(ipproto_ip, ip_pktinfo, data, sizeof(in_pktinfo));
        } else if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_RECVTTL && size >= 1) {
            const int ttl = data[0];
            out.put(ipproto_ip, ip_ttl, &ttl, sizeof ttl);
        } else if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_RECVTOS && size >= 1) {
            out.put(ipproto_ip, ip_tos, data, 1);
        } else if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO && size >= sizeof(in6_pktinfo)) {
            out.put(ipproto_ipv6, ipv6_pktinfo, data, sizeof(in6_pktinfo));
        } else if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_HOPLIMIT && size >= sizeof(int)) {
            out.put(ipproto_ipv6, ipv6_hoplimit, data, sizeof(int));
        } else if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_TCLASS && size >= sizeof(int)) {
            out.put(ipproto_ipv6, ipv6_tclass, data, sizeof(int));
        }
    }
}

// --- Flags -------------------------------------------------------------------------------------------------------

/// The host's flags of a send or receive: those with a host form; the runtime's waits stand for MSG_DONTWAIT and
/// MSG_WAITALL, SO_NOSIGPIPE for MSG_NOSIGNAL.
int host_flags(int flags) {
    int host = 0;
    if (flags & msg_oob) host |= MSG_OOB;
    if (flags & msg_peek) host |= MSG_PEEK;
    if (flags & msg_dontroute) host |= MSG_DONTROUTE;
    if (flags & msg_eor) host |= MSG_EOR;
    return host;
}

/// Linux's msg_flags of a received message's.
std::int32_t guest_flags(int host) {
    std::int32_t flags = 0;
    if (host & MSG_OOB) flags |= msg_oob;
    if (host & MSG_TRUNC) flags |= msg_trunc;
    if (host & MSG_CTRUNC) flags |= msg_ctrunc;
    if (host & MSG_EOR) flags |= msg_eor;
    return flags;
}

// --- Messages ----------------------------------------------------------------------------------------------------

/// The socket behind the guest's `fd`, with its handle, or Linux's answer for none.
struct Held {
    descriptors::Handle handle;
    Socket *socket = nullptr;
    long error = 0;
};

Held hold(int fd) {
    Held held;
    held.handle = descriptors::get(fd);
    if (!held.handle) {
        held.error = -linux_errno::ebadf;
    } else if ((held.socket = as_socket(*held.handle)) == nullptr) {
        held.error = -linux_errno::enotsock;
    }
    return held;
}

/// sendmsg's work for the guest's `message` on `socket`.
long send_message(Socket &socket, const LinuxMsghdr &message, int flags) {
    if (message.vector_count > 1024) return -linux_errno::emsgsize;  // UIO_MAXIOV
    const bool stream = socket.type == sock_stream;
    HostName name;
    const bool named = message.name != 0 && message.name_length > 0;
    if (named && stream && socket.family == af_unix) {
        const std::lock_guard lock(socket.mutex);
        return socket.connected ? -linux_errno::eisconn : -linux_errno::eopnotsupp;
    }
    if (named && !stream) {
        if (const long error = host_name_of(socket, message.name, message.name_length, Use::send, name)) return error;
    }
    HostControl control;
    if (const long error = host_control_of(message, control)) return error;
    std::vector<iovec> rest;
    if (message.vector_count > 0) {
        const auto *vectors = sys::as_pointer<const iovec>(message.vectors);
        rest.assign(vectors, vectors + message.vector_count);
    }
    const bool dont_wait = socket.nonblocking() || (flags & msg_dontwait) != 0;
    msghdr host{};
    if (name.length > 0) {
        host.msg_name = &name.storage;
        host.msg_namelen = name.length;
    }
    long sent = 0;
    bool control_sent = control.bytes.empty();
    while (true) {
        const long result = wait_ready(socket, true, dont_wait, [&] {
            host.msg_iov = rest.data();
            host.msg_iovlen = static_cast<int>(rest.size());
            host.msg_control = control_sent ? nullptr : control.bytes.data();
            host.msg_controllen = control_sent ? 0 : static_cast<socklen_t>(control.bytes.size());
            return in_folder(name.folder, [&] {
                return sys::result_or_error(::sendmsg(socket.host(), &host, host_flags(flags)));
            });
        });
        if (result < 0) {
            if (sent > 0) return sent;
            if (result == -linux_errno::epipe && (flags & msg_nosignal) == 0) sys::raise_sigpipe();
            return result;
        }
        if (!control_sent) {
            control_sent = true;
            control.delivered = true;
        }
        sent += result;
        // What a stream took: the rest goes on, for a guest that waits.
        auto left = static_cast<std::size_t>(result);
        while (!rest.empty() && left >= rest.front().iov_len) {
            left -= rest.front().iov_len;
            rest.erase(rest.begin());
        }
        if (!rest.empty()) {
            rest.front().iov_base = static_cast<char *>(rest.front().iov_base) + left;
            rest.front().iov_len -= left;
        }
        if (!stream || rest.empty() || dont_wait) return sent;
    }
}

/// recvmsg's work for the guest's `message` on `socket`: fills in its control's length and its flags, and `from` with
/// the sender's guest name, empty for none.
long receive_message(Socket &socket, LinuxMsghdr &message, int flags, std::string *from) {
    if (flags & msg_errqueue) return -linux_errno::eagain;  // no error queue is kept
    if (message.vector_count > 1024) return -linux_errno::emsgsize;
    const bool stream = socket.type == sock_stream;
    const bool dont_wait = socket.nonblocking() || (flags & msg_dontwait) != 0;
    std::vector<iovec> rest;
    std::size_t wanted = 0;
    if (message.vector_count > 0) {
        const auto *vectors = sys::as_pointer<const iovec>(message.vectors);
        rest.assign(vectors, vectors + message.vector_count);
        for (const auto &vector : rest) wanted += vector.iov_len;
    }
    // MSG_TRUNC: a datagram's whole length, which Darwin tells before the receive, once a peek found the datagram
    // there (a wait may come first: Darwin's loopback hands a UDP datagram over after its sendto returned).
    const bool want_whole = (flags & msg_trunc) != 0 && !stream;
    int whole = -1;
    sockaddr_storage sender{};
    std::vector<unsigned char> host_control(4096);
    std::int32_t out_flags = 0;
    GuestControl guest_control(message.control, message.control_length, out_flags);
    const bool close_on_exec = (flags & msg_cmsg_cloexec) != 0;
    long received = 0;
    std::string from_name;
    bool have_from = false;
    while (true) {
        msghdr host{};
        const long result = wait_ready(socket, false, dont_wait, [&] {
            if (want_whole) {
                char first;  // a byte: with none, Darwin answers 0 rather than EAGAIN when nothing is there
                iovec probe_vector{&first, 1};
                msghdr probe{};
                probe.msg_iov = &probe_vector;
                probe.msg_iovlen = 1;
                if (const long found = sys::result_or_error(::recvmsg(socket.host(), &probe, MSG_PEEK)); found < 0) {
                    return found;
                }
                socklen_t size = sizeof whole;
                if (::getsockopt(socket.host(), SOL_SOCKET, SO_NREAD, &whole, &size) != 0) whole = -1;
            }
            host = {};
            host.msg_name = &sender;
            host.msg_namelen = sizeof sender;
            host.msg_iov = rest.data();
            host.msg_iovlen = static_cast<int>(rest.size());
            host.msg_control = host_control.data();
            host.msg_controllen = static_cast<socklen_t>(host_control.size());
            return sys::result_or_error(::recvmsg(socket.host(), &host, host_flags(flags)));
        });
        if (result < 0) {
            if (received > 0) break;
            return result;
        }
        guest_control_of(socket, host, guest_control, close_on_exec, (flags & msg_peek) != 0);
        out_flags |= guest_flags(host.msg_flags);
        if (!have_from && host.msg_namelen > 0) {
            from_name = guest_name_of(sender, host.msg_namelen);
            have_from = true;
        }
        received += result;
        const bool waitall = stream && (flags & msg_waitall) != 0 && (flags & msg_peek) == 0;
        if (!waitall || result == 0 || static_cast<std::size_t>(received) >= wanted || dont_wait) {
            if (!stream && whole >= 0 && (out_flags & msg_trunc) != 0 && (flags & msg_trunc) != 0) received = whole;
            break;
        }
        auto left = static_cast<std::size_t>(result);
        while (!rest.empty() && left >= rest.front().iov_len) {
            left -= rest.front().iov_len;
            rest.erase(rest.begin());
        }
        if (!rest.empty()) {
            rest.front().iov_base = static_cast<char *>(rest.front().iov_base) + left;
            rest.front().iov_len -= left;
        }
    }
    // The sender's name: a datagram's; a stream's none (tcp_recvmsg gives none, unix_stream_recvmsg a bound sender's).
    if (stream && socket.family != af_unix) from_name.clear();
    *from = std::move(from_name);
    message.control_length = guest_control.used();
    message.flags = out_flags;
    return received;
}

// --- Options -----------------------------------------------------------------------------------------------------

/// Options Linux takes that change nothing here, kept so that getsockopt reads them back.
enum Kept { kept_tcp_syncnt, kept_tcp_linger2, kept_tcp_defer_accept, kept_tcp_window_clamp, kept_tcp_quickack,
            kept_tcp_user_timeout, kept_tcp_fastopen, kept_ip_recverr };
static_assert(kept_ip_recverr < 8);

int read_int(std::uint64_t value) {
    int result;
    std::memcpy(&result, sys::as_pointer<void>(value), sizeof result);
    return result;
}

/// Reports once that a socket option Linux has is not supported yet.
void report_option(int level, int name) {
    static std::mutex mutex;
    static std::set<std::pair<int, int>> reported;
    const std::lock_guard lock(mutex);
    if (!reported.emplace(level, name).second) return;
    char message[96];
    std::snprintf(message, sizeof message, "apkrt: socket option %d of level %d is not supported yet\n", name, level);
    sys::report(message);
}

/// A Darwin socket option of an int, set from the guest's int.
long set_host_int(Socket &socket, int level, int name, int value) {
    return sys::result_or_error(::setsockopt(socket.host(), level, name, &value, sizeof value));
}

/// getsockopt's copy of an int into the guest's room, Linux's way: as much of it as the room holds.
long put_int(int value, std::uint64_t address, std::uint64_t length_address) {
    std::int32_t room;
    std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
    if (room < 0) return -linux_errno::einval;
    const auto copied = static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(room), sizeof value));
    std::memcpy(sys::as_pointer<void>(address), &value, static_cast<std::size_t>(copied));
    std::memcpy(sys::as_pointer<void>(length_address), &copied, sizeof copied);
    return 0;
}

/// getsockopt's copy of a struct, which needs room for all of it (EINVAL), as SO_RCVTIMEO's.
long put_whole(const void *data, std::size_t size, std::uint64_t address, std::uint64_t length_address) {
    std::int32_t room;
    std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
    if (room < 0 || static_cast<std::size_t>(room) < size) return -linux_errno::einval;
    std::memcpy(sys::as_pointer<void>(address), data, size);
    const auto length = static_cast<std::int32_t>(size);
    std::memcpy(sys::as_pointer<void>(length_address), &length, sizeof length);
    return 0;
}

/// A host option of an int, as Linux reads it back: booleans as 0 or 1 (Darwin answers the option's bit).
int host_int(Socket &socket, int level, int name, bool boolean) {
    int value = 0;
    socklen_t length = sizeof value;
    ::getsockopt(socket.host(), level, name, &value, &length);
    return boolean ? value != 0 : value;
}

/// sock_set_timeout: Linux's struct timeval in jiffies, rounded up; none for 0, at once for a negative time.
long set_timeout(std::atomic<std::int64_t> &timeout, std::uint64_t value, std::uint32_t length) {
    LinuxTimeval time;
    if (length < sizeof time) return -linux_errno::einval;
    std::memcpy(&time, sys::as_pointer<void>(value), sizeof time);
    if (time.microseconds < 0 || time.microseconds >= 1'000'000) return -linux_errno::edom;
    if (time.seconds < 0) {
        timeout.store(-1);
        return 0;
    }
    if (time.seconds == 0 && time.microseconds == 0) {
        timeout.store(0);
        return 0;
    }
    if (time.seconds > INT32_MAX / hz) {
        timeout.store(0);
        return 0;
    }
    constexpr std::int64_t microseconds_per_jiffy = 1'000'000 / hz;
    timeout.store(time.seconds * hz + (time.microseconds + microseconds_per_jiffy - 1) / microseconds_per_jiffy);
    return 0;
}

LinuxTimeval timeout_of(std::int64_t jiffies) {
    if (jiffies <= 0) return {0, 0};
    return {jiffies / hz, (jiffies % hz) * 1'000'000 / hz};
}

/// The bytes of an IP option's value, Linux's way: an int when it has room for one, else a byte.
long ip_value(std::uint64_t value, std::uint32_t length, int &out) {
    if (length >= sizeof(int)) {
        out = read_int(value);
    } else if (length >= 1) {
        out = *sys::as_pointer<const unsigned char>(value);
    } else {
        return -linux_errno::einval;
    }
    return 0;
}

long set_socket_level(Socket &socket, int name, std::uint64_t value, std::uint32_t length) {
    switch (name) {
        case so_rcvtimeo_old:
        case so_rcvtimeo_new: return set_timeout(socket.receive_timeout, value, length);
        case so_sndtimeo_old:
        case so_sndtimeo_new: return set_timeout(socket.send_timeout, value, length);
        case so_linger: {
            linger guest;
            if (length < sizeof guest) return -linux_errno::einval;
            std::memcpy(&guest, sys::as_pointer<void>(value), sizeof guest);
            return sys::result_or_error(::setsockopt(socket.host(), SOL_SOCKET, SO_LINGER_SEC, &guest, sizeof guest));
        }
        case so_bindtodevice:
        case so_mark:
        case so_sndbufforce:
        case so_rcvbufforce: return -linux_errno::eperm;  // CAP_NET_RAW, CAP_NET_ADMIN
        default: break;
    }
    if (length < sizeof(int)) return -linux_errno::einval;
    const int number = read_int(value);
    switch (name) {
        case so_debug: return number != 0 ? -linux_errno::eacces : set_host_int(socket, SOL_SOCKET, SO_DEBUG, 0);
        case so_reuseaddr: return set_host_int(socket, SOL_SOCKET, SO_REUSEADDR, number != 0);
        case so_reuseport: return set_host_int(socket, SOL_SOCKET, SO_REUSEPORT, number != 0);
        case so_keepalive: return set_host_int(socket, SOL_SOCKET, SO_KEEPALIVE, number != 0);
        case so_dontroute: return set_host_int(socket, SOL_SOCKET, SO_DONTROUTE, number != 0);
        case so_broadcast: return set_host_int(socket, SOL_SOCKET, SO_BROADCAST, number != 0);
        case so_oobinline: return set_host_int(socket, SOL_SOCKET, SO_OOBINLINE, number != 0);
        case so_timestamp_old: return set_host_int(socket, SOL_SOCKET, SO_TIMESTAMP, number != 0);
        case so_rcvlowat: return set_host_int(socket, SOL_SOCKET, SO_RCVLOWAT, std::max(number, 1));
        case so_sndlowat: return -linux_errno::enoprotoopt;  // not changeable on Linux
        case so_sndbuf:
        case so_rcvbuf: {
            // Linux keeps twice the value, within wmem_max and rmem_max and a minimum.
            const int kept = std::clamp(number, 0, linux_buffer_max);
            const int doubled = std::max(kept * 2, name == so_sndbuf ? 4608 : 2304);
            const std::lock_guard lock(socket.mutex);
            (name == so_sndbuf ? socket.send_buffer : socket.receive_buffer) = doubled;
            if (socket.family == af_unix && socket.type == sock_dgram) {
                if (name == so_sndbuf) limit_datagrams(socket.host(), doubled);  // the receiver's does not count
            } else {
                ::setsockopt(socket.host(), SOL_SOCKET, name == so_sndbuf ? SO_SNDBUF : SO_RCVBUF, &kept, sizeof kept);
            }
            return 0;
        }
        case so_priority:
            if (number < 0 || number > 6) return -linux_errno::eperm;  // CAP_NET_ADMIN
            {
                const std::lock_guard lock(socket.mutex);
                socket.priority = number;
            }
            return 0;
        case so_passcred: {
            const std::lock_guard lock(socket.mutex);
            socket.pass_credentials = number != 0;
            return 0;
        }
        default: return -linux_errno::enoprotoopt;
    }
}

long get_socket_level(Socket &socket, int name, std::uint64_t value, std::uint64_t length_address) {
    switch (name) {
        case so_type: return put_int(socket.type, value, length_address);
        case so_domain: return put_int(socket.family, value, length_address);
        case so_protocol: return put_int(socket.protocol, value, length_address);
        case so_error: {
            const int error = host_int(socket, SOL_SOCKET, SO_ERROR, false);
            return put_int(error == 0 ? 0 : static_cast<int>(sys::to_linux_errno(error)), value, length_address);
        }
        case so_acceptconn: {
            bool listening;
            {
                const std::lock_guard lock(socket.mutex);
                listening = socket.listening;
            }
            return put_int(listening ? 1 : 0, value, length_address);
        }
        case so_reuseaddr: return put_int(host_int(socket, SOL_SOCKET, SO_REUSEADDR, true), value, length_address);
        case so_reuseport: return put_int(host_int(socket, SOL_SOCKET, SO_REUSEPORT, true), value, length_address);
        case so_keepalive: return put_int(host_int(socket, SOL_SOCKET, SO_KEEPALIVE, true), value, length_address);
        case so_dontroute: return put_int(host_int(socket, SOL_SOCKET, SO_DONTROUTE, true), value, length_address);
        case so_broadcast: return put_int(host_int(socket, SOL_SOCKET, SO_BROADCAST, true), value, length_address);
        case so_oobinline: return put_int(host_int(socket, SOL_SOCKET, SO_OOBINLINE, true), value, length_address);
        case so_timestamp_old: return put_int(host_int(socket, SOL_SOCKET, SO_TIMESTAMP, true), value, length_address);
        case so_debug: return put_int(0, value, length_address);
        case so_rcvlowat: return put_int(host_int(socket, SOL_SOCKET, SO_RCVLOWAT, false), value, length_address);
        case so_sndlowat: return put_int(1, value, length_address);
        case so_sndbuf:
        case so_rcvbuf: {
            int kept;
            {
                const std::lock_guard lock(socket.mutex);
                kept = name == so_sndbuf ? socket.send_buffer : socket.receive_buffer;
            }
            if (kept == 0) kept = host_int(socket, SOL_SOCKET, name == so_sndbuf ? SO_SNDBUF : SO_RCVBUF, false);
            return put_int(kept, value, length_address);
        }
        case so_rcvtimeo_old:
        case so_rcvtimeo_new: {
            const auto time = timeout_of(socket.receive_timeout.load());
            return put_whole(&time, sizeof time, value, length_address);
        }
        case so_sndtimeo_old:
        case so_sndtimeo_new: {
            const auto time = timeout_of(socket.send_timeout.load());
            return put_whole(&time, sizeof time, value, length_address);
        }
        case so_linger: {
            linger host{};
            socklen_t size = sizeof host;
            ::getsockopt(socket.host(), SOL_SOCKET, SO_LINGER_SEC, &host, &size);
            linger guest{host.l_onoff != 0, host.l_linger};
            std::int32_t room;
            std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
            if (room < 0) return -linux_errno::einval;
            const auto copied = static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(room), sizeof guest));
            std::memcpy(sys::as_pointer<void>(value), &guest, static_cast<std::size_t>(copied));
            std::memcpy(sys::as_pointer<void>(length_address), &copied, sizeof copied);
            return 0;
        }
        case so_peercred: {
            // Every peer is this process; a socket without one gives Linux's pid 0 and ids of -1.
            bool connected;
            {
                const std::lock_guard lock(socket.mutex);
                connected = socket.connected && socket.family == af_unix;
            }
            const LinuxUcred credentials = connected ? LinuxUcred{::getpid(), app_uid, app_uid} : LinuxUcred{0, ~0u, ~0u};
            std::int32_t room;
            std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
            if (room < 0) return -linux_errno::einval;
            const auto copied = static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(room), sizeof credentials));
            std::memcpy(sys::as_pointer<void>(value), &credentials, static_cast<std::size_t>(copied));
            std::memcpy(sys::as_pointer<void>(length_address), &copied, sizeof copied);
            return 0;
        }
        case so_priority: {
            const std::lock_guard lock(socket.mutex);
            return put_int(socket.priority, value, length_address);
        }
        case so_passcred: {
            const std::lock_guard lock(socket.mutex);
            return put_int(socket.pass_credentials, value, length_address);
        }
        case so_mark: return put_int(0, value, length_address);
        case so_cookie: {
            struct stat status{};
            ::fstat(socket.host(), &status);  // Darwin's number of the socket, unique while it lives
            const auto cookie = static_cast<std::uint64_t>(status.st_ino);
            return put_whole(&cookie, sizeof cookie, value, length_address);
        }
        default: return -linux_errno::enoprotoopt;
    }
}

int &kept_option(Socket &socket, Kept which) {
    return socket.kept[which];
}

long set_tcp_level(Socket &socket, int name, std::uint64_t value, std::uint32_t length) {
    if (length < sizeof(int)) return -linux_errno::einval;
    const int number = read_int(value);
    const auto keep = [&](Kept which) {
        const std::lock_guard lock(socket.mutex);
        kept_option(socket, which) = number;
        return 0L;
    };
    switch (name) {
        case tcp_nodelay: return set_host_int(socket, IPPROTO_TCP, TCP_NODELAY, number != 0);
        case tcp_maxseg: return set_host_int(socket, IPPROTO_TCP, TCP_MAXSEG, number);
        case tcp_cork: return set_host_int(socket, IPPROTO_TCP, TCP_NOPUSH, number != 0);
        case tcp_keepidle:
            if (number < 1 || number > 32767) return -linux_errno::einval;
            return set_host_int(socket, IPPROTO_TCP, TCP_KEEPALIVE, number);
        case tcp_keepintvl:
            if (number < 1 || number > 32767) return -linux_errno::einval;
            return set_host_int(socket, IPPROTO_TCP, TCP_KEEPINTVL, number);
        case tcp_keepcnt:
            if (number < 1 || number > 127) return -linux_errno::einval;
            return set_host_int(socket, IPPROTO_TCP, TCP_KEEPCNT, number);
        case tcp_notsent_lowat: return set_host_int(socket, IPPROTO_TCP, TCP_NOTSENT_LOWAT, number);
        case tcp_syncnt: return keep(kept_tcp_syncnt);
        case tcp_linger2: return keep(kept_tcp_linger2);
        case tcp_defer_accept: return keep(kept_tcp_defer_accept);
        case tcp_window_clamp: return keep(kept_tcp_window_clamp);
        case tcp_quickack: return keep(kept_tcp_quickack);
        case tcp_user_timeout: return keep(kept_tcp_user_timeout);
        case tcp_fastopen: return keep(kept_tcp_fastopen);
        case tcp_info: report_option(ipproto_tcp, name); return -linux_errno::enoprotoopt;
        default: return -linux_errno::enoprotoopt;
    }
}

long get_tcp_level(Socket &socket, int name, std::uint64_t value, std::uint64_t length_address) {
    const auto kept = [&](Kept which) {
        const std::lock_guard lock(socket.mutex);
        return put_int(kept_option(socket, which), value, length_address);
    };
    switch (name) {
        case tcp_nodelay: return put_int(host_int(socket, IPPROTO_TCP, TCP_NODELAY, true), value, length_address);
        case tcp_maxseg: return put_int(host_int(socket, IPPROTO_TCP, TCP_MAXSEG, false), value, length_address);
        case tcp_cork: return put_int(host_int(socket, IPPROTO_TCP, TCP_NOPUSH, true), value, length_address);
        case tcp_keepidle: return put_int(host_int(socket, IPPROTO_TCP, TCP_KEEPALIVE, false), value, length_address);
        case tcp_keepintvl: return put_int(host_int(socket, IPPROTO_TCP, TCP_KEEPINTVL, false), value, length_address);
        case tcp_keepcnt: return put_int(host_int(socket, IPPROTO_TCP, TCP_KEEPCNT, false), value, length_address);
        case tcp_notsent_lowat:
            return put_int(host_int(socket, IPPROTO_TCP, TCP_NOTSENT_LOWAT, false), value, length_address);
        case tcp_syncnt: return kept(kept_tcp_syncnt);
        case tcp_linger2: return kept(kept_tcp_linger2);
        case tcp_defer_accept: return kept(kept_tcp_defer_accept);
        case tcp_window_clamp: return kept(kept_tcp_window_clamp);
        case tcp_quickack: return kept(kept_tcp_quickack);
        case tcp_user_timeout: return kept(kept_tcp_user_timeout);
        case tcp_fastopen: return kept(kept_tcp_fastopen);
        case tcp_info: report_option(ipproto_tcp, name); return -linux_errno::enoprotoopt;
        default: return -linux_errno::enoprotoopt;
    }
}

long set_ip_level(Socket &socket, int name, std::uint64_t value, std::uint32_t length) {
    const bool v4 = socket.family == af_inet;
    int number = 0;
    switch (name) {
        case ip_add_membership:
        case ip_drop_membership: {
            // struct ip_mreq, or ip_mreqn with an interface index besides, as Linux takes either.
            if (length < sizeof(ip_mreq)) return -linux_errno::einval;
            ip_mreq request;
            std::memcpy(&request, sys::as_pointer<void>(value), sizeof request);
            return sys::result_or_error(::setsockopt(socket.host(), IPPROTO_IP,
                                                     name == ip_add_membership ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                                                     &request, sizeof request));
        }
        case ip_multicast_if: {
            in_addr address{};
            if (length >= sizeof(ip_mreq)) {
                std::memcpy(&address, sys::as_pointer<const unsigned char>(value) + 4, sizeof address);  // imr_address
            } else if (length >= sizeof address) {
                std::memcpy(&address, sys::as_pointer<void>(value), sizeof address);
            } else {
                return -linux_errno::einval;
            }
            return sys::result_or_error(::setsockopt(socket.host(), IPPROTO_IP, IP_MULTICAST_IF, &address, sizeof address));
        }
        case ip_transparent: return -linux_errno::eperm;  // CAP_NET_ADMIN
        default: break;
    }
    if (const long error = ip_value(value, length, number)) return error;
    const auto byte = [&](int host_name) {
        const auto small = static_cast<unsigned char>(number);
        return sys::result_or_error(::setsockopt(socket.host(), IPPROTO_IP, host_name, &small, sizeof small));
    };
    switch (name) {
        case ip_tos:
            if (!v4) {  // Darwin takes no IPv4 options on an AF_INET6 socket; Linux keeps it for v4-mapped peers
                const std::lock_guard lock(socket.mutex);
                socket.ip_tos = number & 0xff;
                return 0;
            }
            return set_host_int(socket, IPPROTO_IP, IP_TOS, number & 0xff);
        case ip_ttl:
            if (number < -1 || number == 0 || number > 255) return -linux_errno::einval;
            return v4 ? set_host_int(socket, IPPROTO_IP, IP_TTL, number == -1 ? 64 : number) : 0;
        case ip_pktinfo: return v4 ? set_host_int(socket, IPPROTO_IP, IP_PKTINFO, number != 0) : 0;
        case ip_recvttl: return v4 ? set_host_int(socket, IPPROTO_IP, IP_RECVTTL, number != 0) : 0;
        case ip_recvtos: return v4 ? set_host_int(socket, IPPROTO_IP, IP_RECVTOS, number != 0) : 0;
        case ip_multicast_ttl: return v4 ? byte(IP_MULTICAST_TTL) : 0;
        case ip_multicast_loop: return v4 ? byte(IP_MULTICAST_LOOP) : 0;
        case ip_multicast_all: {
            const std::lock_guard lock(socket.mutex);
            socket.multicast_all = number != 0;
            return 0;
        }
        case ip_recverr: {
            const std::lock_guard lock(socket.mutex);
            kept_option(socket, kept_ip_recverr) = number != 0;
            return 0;
        }
        case ip_mtu_discover:
        case ip_freebind:
        case ip_bind_address_no_port: return 0;
        default: return -linux_errno::enoprotoopt;
    }
}

long get_ip_level(Socket &socket, int name, std::uint64_t value, std::uint64_t length_address) {
    const bool v4 = socket.family == af_inet;
    // ip_getsockopt: a value of a byte goes as one byte into a room of less than an int.
    const auto put_ip = [&](int number) {
        std::int32_t room;
        std::memcpy(&room, sys::as_pointer<void>(length_address), sizeof room);
        if (room < 0) return -linux_errno::einval;
        if (room > 0 && room < static_cast<std::int32_t>(sizeof(int)) && number >= 0 && number <= 255) {
            *sys::as_pointer<unsigned char>(value) = static_cast<unsigned char>(number);
            const std::int32_t one = 1;
            std::memcpy(sys::as_pointer<void>(length_address), &one, sizeof one);
            return 0L;
        }
        return put_int(number, value, length_address);
    };
    switch (name) {
        case ip_tos: {
            if (!v4) {
                const std::lock_guard lock(socket.mutex);
                return put_ip(socket.ip_tos);
            }
            return put_ip(host_int(socket, IPPROTO_IP, IP_TOS, false));
        }
        case ip_ttl: return put_ip(v4 ? host_int(socket, IPPROTO_IP, IP_TTL, false) : 64);
        case ip_pktinfo: return put_ip(v4 ? host_int(socket, IPPROTO_IP, IP_PKTINFO, true) : 0);
        case ip_recvttl: return put_ip(v4 ? host_int(socket, IPPROTO_IP, IP_RECVTTL, true) : 0);
        case ip_recvtos: return put_ip(v4 ? host_int(socket, IPPROTO_IP, IP_RECVTOS, true) : 0);
        case ip_multicast_ttl: {
            unsigned char small = 1;
            socklen_t size = sizeof small;
            if (v4) ::getsockopt(socket.host(), IPPROTO_IP, IP_MULTICAST_TTL, &small, &size);
            return put_ip(small);
        }
        case ip_multicast_loop: {
            unsigned char small = 1;
            socklen_t size = sizeof small;
            if (v4) ::getsockopt(socket.host(), IPPROTO_IP, IP_MULTICAST_LOOP, &small, &size);
            return put_ip(small);
        }
        case ip_multicast_all: {
            const std::lock_guard lock(socket.mutex);
            return put_ip(socket.multicast_all);
        }
        case ip_recverr: {
            const std::lock_guard lock(socket.mutex);
            return put_ip(kept_option(socket, kept_ip_recverr));
        }
        default: return -linux_errno::enoprotoopt;
    }
}

long set_ipv6_level(Socket &socket, int name, std::uint64_t value, std::uint32_t length) {
    if (socket.family != af_inet6) return -linux_errno::enoprotoopt;
    switch (name) {
        case ipv6_join_group:
        case ipv6_leave_group: {
            if (length < sizeof(ipv6_mreq)) return -linux_errno::einval;
            ipv6_mreq request;
            std::memcpy(&request, sys::as_pointer<void>(value), sizeof request);
            return sys::result_or_error(::setsockopt(socket.host(), IPPROTO_IPV6,
                                                     name == ipv6_join_group ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                                                     &request, sizeof request));
        }
        case ipv6_pktinfo: {
            if (length < sizeof(in6_pktinfo)) return -linux_errno::einval;
            in6_pktinfo info;
            std::memcpy(&info, sys::as_pointer<void>(value), sizeof info);
            return sys::result_or_error(::setsockopt(socket.host(), IPPROTO_IPV6, IPV6_PKTINFO, &info, sizeof info));
        }
        default: break;
    }
    if (length < sizeof(int)) return -linux_errno::einval;
    const int number = read_int(value);
    switch (name) {
        case ipv6_v6only: return set_host_int(socket, IPPROTO_IPV6, IPV6_V6ONLY, number != 0);
        case ipv6_unicast_hops: return set_host_int(socket, IPPROTO_IPV6, IPV6_UNICAST_HOPS, number);
        case ipv6_multicast_if: return set_host_int(socket, IPPROTO_IPV6, IPV6_MULTICAST_IF, number);
        case ipv6_multicast_hops: return set_host_int(socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, number);
        case ipv6_multicast_loop: return set_host_int(socket, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, number != 0);
        case ipv6_recvpktinfo: return set_host_int(socket, IPPROTO_IPV6, IPV6_RECVPKTINFO, number != 0);
        case ipv6_recvhoplimit: return set_host_int(socket, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, number != 0);
        case ipv6_recvtclass: return set_host_int(socket, IPPROTO_IPV6, IPV6_RECVTCLASS, number != 0);
        case ipv6_tclass:
            if (number < -1 || number > 255) return -linux_errno::einval;
            return set_host_int(socket, IPPROTO_IPV6, IPV6_TCLASS, number == -1 ? 0 : number);
        case ipv6_hoplimit: return set_host_int(socket, IPPROTO_IPV6, IPV6_HOPLIMIT, number);
        case ipv6_recverr:
        case ipv6_mtu_discover: return 0;
        default: return -linux_errno::enoprotoopt;
    }
}

long get_ipv6_level(Socket &socket, int name, std::uint64_t value, std::uint64_t length_address) {
    if (socket.family != af_inet6) return -linux_errno::enoprotoopt;
    const auto host = [&](int host_name, bool boolean) {
        return put_int(host_int(socket, IPPROTO_IPV6, host_name, boolean), value, length_address);
    };
    switch (name) {
        case ipv6_v6only: return host(IPV6_V6ONLY, true);
        case ipv6_unicast_hops: return host(IPV6_UNICAST_HOPS, false);
        case ipv6_multicast_if: return host(IPV6_MULTICAST_IF, false);
        case ipv6_multicast_hops: return host(IPV6_MULTICAST_HOPS, false);
        case ipv6_multicast_loop: return host(IPV6_MULTICAST_LOOP, true);
        case ipv6_recvpktinfo: return host(IPV6_RECVPKTINFO, true);
        case ipv6_recvhoplimit: return host(IPV6_RECVHOPLIMIT, true);
        case ipv6_recvtclass: return host(IPV6_RECVTCLASS, true);
        case ipv6_tclass: return host(IPV6_TCLASS, false);
        default: return -linux_errno::enoprotoopt;
    }
}

}  // namespace

// --- The socket --------------------------------------------------------------------------------------------------

Socket::Socket(int host, bool nonblocking, int family_number, int type_number, int protocol_number)
    : Description(host, true, false, nonblocking), family(family_number), type(type_number), protocol(protocol_number) {
    const bool tcp = type == sock_stream && family != af_unix;
    send_buffer = tcp ? tcp_send_buffer : linux_buffer;
    receive_buffer = tcp ? tcp_receive_buffer : linux_buffer;
    if (family == af_unix && type == sock_dgram) {
        // unix_dgram_sendmsg limits a datagram by the sender's buffer alone, where Darwin also refuses one that does
        // not fit the receiver's (net.local.dgram.recvspace, 4096) with ENOBUFS: room for the longest that Linux
        // takes, after SO_SNDBUF's most (twice wmem_max, less 32 bytes), twice, or what the host's
        // kern.ipc.maxsockbuf allows.
        limit_datagrams(host, send_buffer);
        for (int room = 4 * linux_buffer_max; room >= linux_buffer; room /= 2) {
            if (::setsockopt(host, SOL_SOCKET, SO_RCVBUF, &room, sizeof room) == 0) break;
        }
    }
}

Socket::~Socket() {
    if (host_name.empty()) return;
    auto &names = unix_names();
    const std::lock_guard lock(names.mutex);
    names.guest_of_host.erase(host_name);
    if (abstract) {
        names.host_of_abstract.erase(name);
        if (names.folder >= 0) ::unlinkat(names.folder, host_name.c_str(), 0);
    }
}

Socket *as_socket(descriptors::Description &description) {
    return dynamic_cast<Socket *>(&description);
}

const Socket *as_socket(const descriptors::Description &description) {
    return dynamic_cast<const Socket *>(&description);
}

std::int64_t deadline_of(descriptors::Description &description, bool writing) {
    const auto *socket = as_socket(description);
    if (socket == nullptr) return sys::no_deadline;
    const auto jiffies = (writing ? socket->send_timeout : socket->receive_timeout).load();
    if (jiffies == 0) return sys::no_deadline;
    if (jiffies < 0) return sys::monotonic_now();
    return sys::deadline_after(jiffies * (1'000'000'000 / hz));
}

bool restarts(std::uint64_t number, int fd) {
    bool receiving;
    switch (number) {
        case 63:   // read
        case 65:   // readv
        case 202:  // accept
        case 207:  // recvfrom
        case 212:  // recvmsg
        case 242:  // accept4
        case 243:  // recvmmsg
            receiving = true;
            break;
        case 64:   // write
        case 66:   // writev
        case 203:  // connect
        case 206:  // sendto
        case 211:  // sendmsg
        case 269:  // sendmmsg
            receiving = false;
            break;
        default: return true;
    }
    const auto description = descriptors::get(fd);
    const auto *socket = description ? as_socket(*description) : nullptr;
    if (socket == nullptr) return true;
    return (receiving ? socket->receive_timeout : socket->send_timeout).load() == 0;
}

std::uint32_t poll_bits(Socket &socket, bool read_filter, bool end, std::uint32_t readable, std::uint32_t writable,
                        std::uint32_t hangup) {
    std::uint32_t bits = read_filter ? readable : writable;
    if (!end) return bits;
    // The other direction's end, which the other filter shows: Darwin's poll of it alone reports POLLHUP for its EV_EOF.
    pollfd other{socket.host(), static_cast<short>(read_filter ? POLLOUT : POLLIN), 0};
    const bool other_end = ::poll(&other, 1, 0) == 1 && (other.revents & POLLHUP) != 0;
    const bool read_end = read_filter || other_end, send_end = !read_filter || other_end;
    if (read_end) bits |= readable | pollrdhup;
    if (read_end && send_end) bits |= hangup;
    return bits;
}

}  // namespace apkrt::sockets

// --- The syscalls ------------------------------------------------------------------------------------------------

namespace apkrt::sys {

using sockets::Socket;
using namespace sockets::linux_net;

long sys_socket(int domain, int type, int protocol) {
    if (((type & ~type_mask) & ~(sock_nonblock | sock_cloexec)) != 0) return -linux_errno::einval;
    int base = type & type_mask;
    if (domain < 0 || domain >= af_max) return -linux_errno::eafnosupport;
    if (base >= sock_max) return -linux_errno::einval;
    int host_family, host_protocol = 0, reported = 0;
    switch (domain) {
        case af_unix:
            if (protocol != 0 && protocol != af_unix) return -linux_errno::eprotonosupport;
            if (base == sock_raw) base = sock_dgram;  // unix_create
            if (base != sock_stream && base != sock_dgram && base != sock_seqpacket) return -linux_errno::esocktnosupport;
            host_family = AF_UNIX;
            break;
        case af_inet:
        case af_inet6:
            host_family = domain == af_inet ? AF_INET : AF_INET6;
            if (base == sock_stream) {
                if (protocol != 0 && protocol != ipproto_tcp) return -linux_errno::eprotonosupport;
                reported = host_protocol = ipproto_tcp;
            } else if (base == sock_dgram) {
                if (protocol == 0 || protocol == ipproto_udp) {
                    reported = host_protocol = ipproto_udp;
                } else if ((domain == af_inet && protocol == ipproto_icmp) ||
                           (domain == af_inet6 && protocol == ipproto_icmpv6)) {
                    reported = host_protocol = protocol;  // a ping socket
                } else {
                    return -linux_errno::eprotonosupport;
                }
            } else if (base == sock_raw) {
                return -linux_errno::eperm;  // CAP_NET_RAW
            } else {
                return -linux_errno::esocktnosupport;
            }
            break;
        case af_netlink: {
            static std::once_flag once;
            std::call_once(once, [] { report("apkrt: netlink sockets are not supported yet (ADR-0054 step c)\n"); });
            return -linux_errno::eafnosupport;
        }
        case af_packet: return -linux_errno::eperm;  // CAP_NET_RAW
        default: return -linux_errno::eafnosupport;
    }
    const int host = ::socket(host_family, base, host_protocol);
    if (host < 0) return -to_linux_errno(errno);
    if (const long error = sockets::prepare(host)) return error;
    return descriptors::add(std::make_shared<Socket>(host, (type & sock_nonblock) != 0, domain, base, reported),
                            (type & sock_cloexec) != 0);
}

long sys_socketpair(int domain, int type, int protocol, std::uint64_t vector) {
    if (((type & ~type_mask) & ~(sock_nonblock | sock_cloexec)) != 0) return -linux_errno::einval;
    int base = type & type_mask;
    if (domain < 0 || domain >= af_max) return -linux_errno::eafnosupport;
    if (domain != af_unix) return domain == af_inet || domain == af_inet6 ? -linux_errno::eopnotsupp : -linux_errno::eafnosupport;
    if (protocol != 0 && protocol != af_unix) return -linux_errno::eprotonosupport;
    if (base == sock_raw) base = sock_dgram;
    if (base != sock_stream && base != sock_dgram && base != sock_seqpacket) return -linux_errno::esocktnosupport;
    int hosts[2];
    if (::socketpair(AF_UNIX, base, 0, hosts) != 0) return -to_linux_errno(errno);
    if (const long error = sockets::prepare(hosts[0])) {
        ::close(hosts[1]);
        return error;
    }
    if (const long error = sockets::prepare(hosts[1])) {
        ::close(hosts[0]);
        return error;
    }
    const bool nonblocking = (type & sock_nonblock) != 0, close_on_exec = (type & sock_cloexec) != 0;
    auto first = std::make_shared<Socket>(hosts[0], nonblocking, af_unix, base, 0);
    auto second = std::make_shared<Socket>(hosts[1], nonblocking, af_unix, base, 0);
    first->connected = second->connected = true;
    const long one = descriptors::add(std::move(first), close_on_exec);
    if (one < 0) return one;  // the second goes with its handle
    const long other = descriptors::add(std::move(second), close_on_exec);
    if (other < 0) {
        descriptors::close(static_cast<int>(one));
        return other;
    }
    const std::int32_t fds[2] = {static_cast<std::int32_t>(one), static_cast<std::int32_t>(other)};
    std::memcpy(as_pointer<void>(vector), fds, sizeof fds);
    return 0;
}

namespace {

long bind_unix(Socket &socket, sockets::HostName &name) {
    const std::lock_guard lock(socket.mutex);
    if (!socket.name.empty()) return -linux_errno::einval;  // bound already
    auto &names = sockets::unix_names();
    const std::lock_guard names_lock(names.mutex);
    ++names.made;
    const std::string host_name = sockets::unguessable_name(name.guest.empty() || name.guest[2] == '\0' ? "a" : ".apkrt-");
    sockaddr_un host{};
    const socklen_t length = sockets::host_unix_name(host_name, host);
    // The host's bind follows a link where it binds: the name is unguessable, and what it made is checked.
    const auto bind_in = [&](int folder) {
        const long result = sockets::in_folder(folder, [&] {
            return result_or_error(::bind(socket.host(), reinterpret_cast<const sockaddr *>(&host), length));
        });
        if (result != 0) return result;
        struct stat status{};
        if (::fstatat(folder, host_name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(status.st_mode)) {
            return -linux_errno::eacces;
        }
        return 0L;
    };
    if (name.autobind || name.guest[2] == '\0') {
        if (name.autobind) {
            // unix_autobind: an abstract name of five hex digits.
            for (std::uint32_t number = static_cast<std::uint32_t>(names.made);; ++number) {
                char digits[8];
                std::snprintf(digits, sizeof digits, "%05x", number & 0xfffff);
                std::string guest = sockets::unnamed;
                guest += '\0';
                guest += digits;
                if (names.host_of_abstract.count(guest) == 0) {
                    name.guest = guest;
                    break;
                }
            }
        }
        if (names.host_of_abstract.count(name.guest) != 0) return -linux_errno::eaddrinuse;
        const int folder = sockets::abstract_folder(names);
        if (folder < 0) return -linux_errno::eacces;
        if (const long result = bind_in(folder)) return result;
        names.host_of_abstract[name.guest] = host_name;
        socket.abstract = true;
    } else {
        // A name of its own first, which Darwin keeps for getpeername, accept and recvfrom; then it takes the guest's
        // name in its folder, unless something took that meanwhile.
        const int folder = name.place.folder;
        if (const long result = bind_in(folder)) return result;
        if (::renameatx_np(folder, host_name.c_str(), folder, name.place.name.c_str(), RENAME_EXCL) != 0) {
            const int error = errno;
            ::unlinkat(folder, host_name.c_str(), 0);
            return error == EEXIST ? -linux_errno::eaddrinuse : -to_linux_errno(error);
        }
    }
    names.guest_of_host[host_name] = name.guest;
    socket.name = name.guest;
    socket.host_name = host_name;
    return 0;
}

}  // namespace

long sys_bind(int fd, std::uint64_t address, std::uint64_t length) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    sockets::HostName name;
    if (const long error = sockets::host_name_of(socket, address, length, sockets::Use::bind, name)) return error;
    if (socket.family == af_unix) return bind_unix(socket, name);
    return result_or_error(::bind(socket.host(), name.address(), name.length));
}

long sys_listen(int fd, int backlog) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (socket.type == sock_dgram) return -linux_errno::eopnotsupp;
    if (socket.family == af_unix) {
        const std::lock_guard lock(socket.mutex);
        if (socket.name.empty()) return -linux_errno::einval;  // unix_listen: no listens on an unbound socket
    }
    const long result = result_or_error(::listen(socket.host(), std::clamp(backlog, 0, 4096)));
    if (result == 0) {
        const std::lock_guard lock(socket.mutex);
        socket.listening = true;
    }
    return result;
}

long sys_accept4(int fd, std::uint64_t address, std::uint64_t length_address, int flags) {
    if ((flags & ~(sock_nonblock | sock_cloexec)) != 0) return -linux_errno::einval;
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (socket.type == sock_dgram) return -linux_errno::eopnotsupp;
    sockaddr_storage peer{};
    socklen_t peer_length = 0;
    int accepted = -1;
    const long result = sockets::wait_ready(socket, false, socket.nonblocking(), [&] {
        peer_length = sizeof peer;
        accepted = ::accept(socket.host(), reinterpret_cast<sockaddr *>(&peer), &peer_length);
        return accepted >= 0 ? 0L : -to_linux_errno(errno);
    });
    if (result < 0) return result;
    if (const long error = sockets::prepare(accepted)) return error;
    auto made = std::make_shared<Socket>(accepted, (flags & sock_nonblock) != 0, socket.family, socket.type,
                                         socket.protocol);
    std::string peer_name = sockets::guest_name_of(peer, peer_length);
    made->connected = true;
    if (socket.family == af_unix) {
        const std::lock_guard lock(socket.mutex);
        made->name = socket.name;  // an accepted socket has its listener's name
        made->peer = peer_name;
        if (peer_name.empty()) peer_name = sockets::unnamed;
    }
    const long guest = descriptors::add(std::move(made), (flags & sock_cloexec) != 0);
    if (guest < 0 || address == 0) return guest;
    if (const long error = sockets::put_name(peer_name, address, length_address)) {
        descriptors::close(static_cast<int>(guest));
        return error;
    }
    return guest;
}

long sys_connect(int fd, std::uint64_t address, std::uint64_t length) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (socket.family == af_unix && socket.type == sock_dgram && length >= 2 && address != 0 &&
        sockets::read_u16(as_pointer<const unsigned char>(address)) == af_unspec) {
        // unix_dgram_connect: AF_UNSPEC ends the association.
        sockaddr unspecified{};
        unspecified.sa_len = sizeof unspecified;
        unspecified.sa_family = AF_UNSPEC;
        ::connect(socket.host(), &unspecified, sizeof unspecified);
        const std::lock_guard lock(socket.mutex);
        socket.connected = false;
        socket.peer.clear();
        return 0;
    }
    sockets::HostName name;
    if (const long error = sockets::host_name_of(socket, address, length, sockets::Use::connect, name)) return error;
    if (name.unspecified) {
        sockaddr unspecified{};
        unspecified.sa_len = sizeof unspecified;
        unspecified.sa_family = AF_UNSPEC;
        ::connect(socket.host(), &unspecified, sizeof unspecified);  // Darwin dissolves the association, then answers
        const std::lock_guard lock(socket.mutex);
        socket.connected = false;
        return 0;
    }
    if (socket.family != af_unix) {
        return sockets::connect_wait(socket, [&] { return result_or_error(::connect(socket.host(), name.address(), name.length)); });
    }
    const long result = sockets::in_folder(name.folder, [&] {
        return result_or_error(::connect(socket.host(), name.address(), name.length));
    });
    if (result != 0) return result;
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof peer;
    std::string peer_name;
    if (::getpeername(socket.host(), reinterpret_cast<sockaddr *>(&peer), &peer_length) == 0) {
        peer_name = sockets::guest_name_of(peer, peer_length);
    }
    const std::lock_guard lock(socket.mutex);
    socket.peer = peer_name.empty() ? name.guest : peer_name;
    socket.connected = true;
    return 0;
}

long sys_getsockname(int fd, std::uint64_t address, std::uint64_t length_address) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (socket.family == af_unix) {
        std::string name;
        {
            const std::lock_guard lock(socket.mutex);
            name = socket.name.empty() ? sockets::unnamed : socket.name;
        }
        return sockets::put_name(name, address, length_address);
    }
    sockaddr_storage host{};
    socklen_t length = sizeof host;
    if (::getsockname(socket.host(), reinterpret_cast<sockaddr *>(&host), &length) != 0) return -to_linux_errno(errno);
    if (host.ss_family == AF_UNSPEC || length == 0) host.ss_family = static_cast<sa_family_t>(socket.family == af_inet ? AF_INET : AF_INET6);
    return sockets::put_name(sockets::guest_name_of(host, length), address, length_address);
}

long sys_getpeername(int fd, std::uint64_t address, std::uint64_t length_address) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (socket.family == af_unix) {
        std::string name;
        {
            const std::lock_guard lock(socket.mutex);
            if (!socket.connected) return -linux_errno::enotconn;
            name = socket.peer.empty() ? sockets::unnamed : socket.peer;
        }
        return sockets::put_name(name, address, length_address);
    }
    sockaddr_storage host{};
    socklen_t length = sizeof host;
    if (::getpeername(socket.host(), reinterpret_cast<sockaddr *>(&host), &length) != 0) return -to_linux_errno(errno);
    return sockets::put_name(sockets::guest_name_of(host, length), address, length_address);
}

long sys_shutdown(int fd, int how) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    if (how < 0 || how > 2) return -linux_errno::einval;
    if (::shutdown(held.socket->host(), how) == 0) return 0;
    // unix_shutdown answers 0 also for an unconnected socket.
    if (errno == ENOTCONN && held.socket->family == af_unix) return 0;
    return -to_linux_errno(errno);
}

long sys_sendmsg(int fd, std::uint64_t message, int flags) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    sockets::LinuxMsghdr header;
    std::memcpy(&header, as_pointer<void>(message), sizeof header);
    return sockets::send_message(*held.socket, header, flags);
}

long sys_sendto(int fd, std::uint64_t buffer, std::uint64_t length, int flags, std::uint64_t address,
                std::uint64_t address_length) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    iovec one{as_pointer<void>(buffer), length};
    sockets::LinuxMsghdr header{};
    header.name = address;
    header.name_length = static_cast<std::uint32_t>(address == 0 ? 0 : address_length);
    header.vectors = reinterpret_cast<std::uint64_t>(&one);
    header.vector_count = 1;
    return sockets::send_message(*held.socket, header, flags);
}

namespace {

/// The sender's name of a received message into the guest's msg_name, as Linux writes it: cut to the room, with its
/// whole length in msg_namelen; none for an unnamed sender.
void put_message_name(sockets::LinuxMsghdr &header, std::string_view name, std::uint32_t room) {
    if (header.name == 0) {
        header.name_length = 0;
        return;
    }
    const std::size_t copied = std::min<std::size_t>(room, name.size());
    if (copied > 0) std::memcpy(as_pointer<void>(header.name), name.data(), copied);
    header.name_length = static_cast<std::uint32_t>(name.size());
}

}  // namespace

long sys_recvmsg(int fd, std::uint64_t message, int flags) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    auto *guest = as_pointer<sockets::LinuxMsghdr>(message);
    sockets::LinuxMsghdr header;
    std::memcpy(&header, guest, sizeof header);
    const std::uint32_t room = header.name_length;
    std::string from;
    const long result = sockets::receive_message(*held.socket, header, flags, &from);
    if (result < 0) return result;
    put_message_name(header, from, room);
    guest->name_length = header.name_length;
    guest->control_length = header.control_length;
    guest->flags = header.flags;
    return result;
}

long sys_recvfrom(int fd, std::uint64_t buffer, std::uint64_t length, int flags, std::uint64_t address,
                  std::uint64_t length_address) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    iovec one{as_pointer<void>(buffer), length};
    sockets::LinuxMsghdr header{};
    header.vectors = reinterpret_cast<std::uint64_t>(&one);
    header.vector_count = 1;
    std::string from;
    const long result = sockets::receive_message(*held.socket, header, flags, &from);
    if (result < 0 || address == 0) return result;
    if (const long error = sockets::put_name(from, address, length_address)) return error;
    return result;
}

long sys_sendmmsg(int fd, std::uint64_t vector, std::uint32_t count, int flags) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    count = std::min<std::uint32_t>(count, 1024);  // UIO_MAXIOV
    auto *messages = as_pointer<sockets::LinuxMmsghdr>(vector);
    std::uint32_t sent = 0;
    for (; sent < count; ++sent) {
        const long result = sockets::send_message(*held.socket, messages[sent].header, flags);
        if (result < 0) return sent > 0 ? static_cast<long>(sent) : result;
        messages[sent].length = static_cast<std::uint32_t>(result);
    }
    return static_cast<long>(sent);
}

long sys_recvmmsg(int fd, std::uint64_t vector, std::uint32_t count, int flags, std::uint64_t timeout) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    std::int64_t deadline = no_deadline;
    if (timeout != 0) {
        timespec time;
        std::memcpy(&time, as_pointer<void>(timeout), sizeof time);
        if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1'000'000'000) return -linux_errno::einval;
        deadline = deadline_after(nanoseconds_of(time));
    }
    auto *messages = as_pointer<sockets::LinuxMmsghdr>(vector);
    std::uint32_t received = 0;
    for (; received < count; ++received) {
        int each = flags & ~msg_waitforone;
        if ((flags & msg_waitforone) != 0 && received > 0) each |= msg_dontwait;
        auto &header = messages[received].header;
        const std::uint32_t room = header.name_length;
        std::string from;
        const long result = sockets::receive_message(*held.socket, header, each, &from);
        if (result < 0) return received > 0 ? static_cast<long>(received) : result;
        put_message_name(header, from, room);
        messages[received].length = static_cast<std::uint32_t>(result);
        if (deadline != no_deadline && monotonic_now() >= deadline) {
            ++received;
            break;
        }
    }
    return static_cast<long>(received);
}

long sys_setsockopt(int fd, int level, int name, std::uint64_t value, std::uint32_t length) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (static_cast<std::int32_t>(length) < 0) return -linux_errno::einval;
    if (length > 0 && value == 0) return -linux_errno::efault;
    if (level == sol_socket) return sockets::set_socket_level(socket, name, value, length);
    if (socket.family == af_unix) return -linux_errno::eopnotsupp;  // AF_UNIX has no options beyond SOL_SOCKET
    switch (level) {
        case ipproto_tcp:
            return socket.type == sock_stream ? sockets::set_tcp_level(socket, name, value, length) : -linux_errno::enoprotoopt;
        case ipproto_ip: return sockets::set_ip_level(socket, name, value, length);
        case ipproto_ipv6: return sockets::set_ipv6_level(socket, name, value, length);
        default: return -linux_errno::enoprotoopt;
    }
}

long sys_getsockopt(int fd, int level, int name, std::uint64_t value, std::uint64_t length_address) {
    const auto held = sockets::hold(fd);
    if (held.error != 0) return held.error;
    Socket &socket = *held.socket;
    if (length_address == 0) return -linux_errno::efault;
    std::int32_t room;
    std::memcpy(&room, sys::as_pointer<const void>(length_address), sizeof room);
    if (room > 0 && value == 0) return -linux_errno::efault;  // a value with no buffer for it, as setsockopt's
    if (level == sol_socket) return sockets::get_socket_level(socket, name, value, length_address);
    if (socket.family == af_unix) return -linux_errno::eopnotsupp;
    switch (level) {
        case ipproto_tcp:
            return socket.type == sock_stream ? sockets::get_tcp_level(socket, name, value, length_address)
                                              : -linux_errno::enoprotoopt;
        case ipproto_ip: return sockets::get_ip_level(socket, name, value, length_address);
        case ipproto_ipv6: return sockets::get_ipv6_level(socket, name, value, length_address);
        default: return -linux_errno::enoprotoopt;
    }
}

}  // namespace apkrt::sys
