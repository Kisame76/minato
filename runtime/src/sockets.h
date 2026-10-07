// The guest's sockets (ADR-0054): a socket is a description whose host descriptor is a Darwin socket, non-blocking
// and with SO_NOSIGPIPE, so that the guest's calls wait in the runtime (ADR-0027, decision 5) and its SIGPIPE comes
// from Linux's rules. sys_sockets.cpp has the syscalls and the translations of Linux's names, flags, options and
// ancillary data to Darwin's and back; sys_files.cpp, sys_events.cpp and signals.cpp ask the hooks below.
#pragma once

#include "descriptors.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace apkrt::sockets {

/// Linux's HZ on Android 15's kernel: SO_RCVTIMEO and SO_SNDTIMEO keep their time in jiffies of it, and read it back
/// so (socket-test, 150 ms reads back as 152 ms).
constexpr std::int64_t hz = 250;

class Socket final : public descriptors::Description {
public:
    /// Takes `host`, a Darwin socket, as one of Linux's `family`, `type` and `protocol`, the one getsockopt reports:
    /// IPPROTO_TCP for a stream of AF_INET or AF_INET6, IPPROTO_UDP for a datagram, 0 for AF_UNIX.
    Socket(int host, bool nonblocking, int family, int type, int protocol);
    ~Socket() override;

    const int family, type, protocol;

    /// SO_RCVTIMEO and SO_SNDTIMEO in jiffies: 0 is none.
    std::atomic<std::int64_t> receive_timeout{0}, send_timeout{0};

    std::mutex mutex;  // for what follows
    /// AF_UNIX: the guest's name it is bound to (Linux's sockaddr bytes), and its host name, the key of the
    /// registry of names (sys_sockets.cpp); the peer's guest name once connected, empty for an unnamed peer.
    std::string name, host_name, peer;
    bool abstract = false;     // host_name is a file of the runtime's folder of abstract names, which goes at close
    bool connecting = false;   // a connect answered EINPROGRESS, and no connect has seen its end since
    bool connected = false;    // a connect, or the accept or socketpair that made it, ended in a connection
    bool listening = false;    // a listen succeeded: SO_ACCEPTCONN, which Darwin's getsockopt does not answer
    bool pass_credentials = false;  // SO_PASSCRED
    int priority = 0;          // SO_PRIORITY
    int ip_tos = 0;            // IP_TOS of an AF_INET6 socket, which Darwin does not take there
    bool multicast_all = true; // IP_MULTICAST_ALL
    int receive_buffer = 0, send_buffer = 0;  // what SO_RCVBUF and SO_SNDBUF were set to, 0 for not set
    int kept[8] = {};          // options Linux takes that change nothing here (sys_sockets.cpp, kept_options)
};

/// The socket behind `description`, or null.
Socket *as_socket(descriptors::Description &description);
const Socket *as_socket(const descriptors::Description &description);

/// The deadline of a wait for `writing` or reading on `description` from now, by SO_SNDTIMEO or SO_RCVTIMEO; none
/// for a description that is no socket or has no timeout.
std::int64_t deadline_of(descriptors::Description &description, bool writing);

/// Whether syscall `number` on guest descriptor `fd`, interrupted by a signal, starts again after a handler with
/// SA_RESTART: never a socket's call while the socket has a timeout for its direction (signal(7)).
bool restarts(std::uint64_t number, int fd);

/// The poll bits a socket's kqueue filter stands for when it fires (sys_events.cpp): `readable` or `writable`, and
/// with `end` (EV_EOF) Linux's EPOLLRDHUP once the peer sends no more, EPOLLHUP once neither direction is open.
std::uint32_t poll_bits(Socket &socket, bool read_filter, bool end, std::uint32_t readable, std::uint32_t writable,
                        std::uint32_t hangup);

/// Linux's EPOLLRDHUP.
constexpr std::uint32_t pollrdhup = 0x2000;

/// Where an AF_UNIX name in the file system lies on the host (sys_files.cpp): the folder of its last component,
/// opened below its mount, and that component's name, or a negative Linux errno.
struct Place {
    long error = 0;
    int folder = -1;  // closed with the Place
    std::string name;

    Place() = default;
    Place(Place &&other) noexcept : error(other.error), folder(other.folder), name(std::move(other.name)) {
        other.folder = -1;
    }
    Place &operator=(Place &&other) noexcept;
    ~Place();
};

/// The place of the guest's `path`: with `creating` for bind, with Linux's answers for making the entry (EADDRINUSE
/// for one that is there, EROFS, EACCES); else for connect and sendto, those for reaching a socket (ENOENT, EACCES
/// without write permission, ECONNREFUSED for no socket), with a final link followed, below the guest's file system.
Place place_of(std::string_view path, bool creating);

}  // namespace apkrt::sockets

namespace apkrt::sys {

// The syscalls (sys_sockets.cpp), with Linux's arguments; each answers as Linux does, a negative errno on failure.
long sys_socket(int domain, int type, int protocol);
long sys_socketpair(int domain, int type, int protocol, std::uint64_t vector);
long sys_bind(int fd, std::uint64_t address, std::uint64_t length);
long sys_listen(int fd, int backlog);
long sys_accept4(int fd, std::uint64_t address, std::uint64_t length_address, int flags);
long sys_connect(int fd, std::uint64_t address, std::uint64_t length);
long sys_getsockname(int fd, std::uint64_t address, std::uint64_t length_address);
long sys_getpeername(int fd, std::uint64_t address, std::uint64_t length_address);
long sys_sendto(int fd, std::uint64_t buffer, std::uint64_t length, int flags, std::uint64_t address,
                std::uint64_t address_length);
long sys_recvfrom(int fd, std::uint64_t buffer, std::uint64_t length, int flags, std::uint64_t address,
                  std::uint64_t length_address);
long sys_setsockopt(int fd, int level, int name, std::uint64_t value, std::uint32_t length);
long sys_getsockopt(int fd, int level, int name, std::uint64_t value, std::uint64_t length_address);
long sys_shutdown(int fd, int how);
long sys_sendmsg(int fd, std::uint64_t message, int flags);
long sys_recvmsg(int fd, std::uint64_t message, int flags);
long sys_sendmmsg(int fd, std::uint64_t vector, std::uint32_t count, int flags);
long sys_recvmmsg(int fd, std::uint64_t vector, std::uint32_t count, int flags, std::uint64_t timeout);

}  // namespace apkrt::sys
