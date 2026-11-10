// The guest's sockets (ADR-0054): a socket is a description whose host descriptor is a Darwin socket, non-blocking
// and with SO_NOSIGPIPE, so that the guest's calls wait in the runtime (ADR-0027, decision 5) and its SIGPIPE comes
// from Linux's rules. sys_sockets.cpp has the syscalls and the translations of Linux's names, flags, options and
// ancillary data to Darwin's and back; sys_files.cpp, sys_events.cpp and signals.cpp ask the hooks below.
#pragma once

#include "descriptors.h"

#include <sys/uio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace apkrt::sockets {

/// Linux's HZ on Android 15's kernel: SO_RCVTIMEO and SO_SNDTIMEO keep their time in jiffies of it, and read it back
/// so (socket-test, 150 ms reads back as 152 ms).
constexpr std::int64_t hz = 250;

class Socket;

/// What the two ends of a SOCK_SEQPACKET socketpair share, which the runtime builds on a Darwin datagram pair (ADR-0054
/// step a2): what Darwin does not tell the other end. Each end is a side, 0 or 1. An end that receives no more gets
/// `mark` queued last, a message of the runtime's that stays: it wakes a call that waits, as Darwin wakes none for a
/// datagram peer's close or shutdown, and keeps the end readable, as Linux's is.
struct SeqpacketPair {
    std::mutex mutex;  // for what follows; a send checks and sends under it, as Linux under the peer's lock
    std::mutex receiving[2];  // a side's receives one at a time, as Linux's u->iolock: no two see the same head
    bool shut_read[2] = {}, shut_write[2] = {}, closed[2] = {};
    bool reset[2] = {};  // the side's next call gets ECONNRESET: the other closed with messages it never read
    unsigned char mark[16] = {};      // random, so that no message of the guest's is the mark
    std::weak_ptr<Socket> ends[2];
};

/// What an AF_NETLINK socket keeps (ADR-0054 step c): its host is one end of a Darwin datagram pair, and the
/// runtime writes the kernel's answers into the other end, `replies`. Guarded by the socket's mutex.
struct NetlinkEnd {
    int replies = -1;           // closed with the socket
    std::uint32_t port = 0;     // its port once a send or connect bound it, 0 before
    std::uint64_t groups = 0;   // the multicast groups NETLINK_ADD_MEMBERSHIP joined: bit n - 1 for group n
    bool joined = false;        // a membership was asked for: NETLINK_LIST_MEMBERSHIPS counts 36 groups since
    std::uint32_t options = 0;  // SOL_NETLINK's flags that are set: bit n for option n
    bool overrun = false;       // answers did not fit the queue: the next receive answers ENOBUFS (netlink_overrun)
};

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
    /// net-option-late: TCP_NODELAY, SO_KEEPALIVE and SO_LINGER that Darwin refuses to set once the connection has
    /// closed (EINVAL) but Linux keeps. A refused set is remembered here and read back; `*_set` is cleared when Darwin
    /// takes the option, so an open socket stays host-authoritative.
    bool late_nodelay_set = false, late_nodelay = false;
    bool late_keepalive_set = false, late_keepalive = false;
    bool late_linger_set = false, late_linger_on = false;
    int late_linger_seconds = 0;

    std::shared_ptr<SeqpacketPair> pair;  // a SOCK_SEQPACKET socketpair's, else none; set before the socket is shared
    int side = 0;

    std::unique_ptr<NetlinkEnd> netlink;  // an AF_NETLINK socket's, else none; set before the socket is shared
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

/// Linux's EPOLLRDHUP, and EPOLLERR, which poll's POLLERR equals.
constexpr std::uint32_t pollrdhup = 0x2000, pollerr = 0x8;

/// Whether EPIPE from a write on guest descriptor `fd` raises SIGPIPE: not for a SOCK_SEQPACKET socket, whose send
/// never raises it on Linux (unix_dgram_sendmsg).
bool raises_sigpipe(int fd);

/// The socket ioctls of the runtime's own (sys_files.cpp's ioctl asks first): the interfaces' ifreq ioctls
/// (netlink.h), and SELinux's refusals for an app on a netlink or AF_UNIX socket. `handled` is false for the rest.
long socket_ioctl(Socket &socket, std::uint32_t request, std::uint64_t argument, bool &handled);

/// Whether read and write on `description` go as messages through the socket's own receive and send: a
/// SOCK_SEQPACKET or AF_NETLINK socket's (transfer_message).
bool takes_messages(const descriptors::Description &description);

/// read, readv, write or writev of `vectors` on such a socket, as recv and send without flags.
long transfer_message(descriptors::Description &description, bool writing, const iovec *vectors, int count);

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

/// Tests only: called with a socket's folder and name once place_of has found a socket there for connect or sendto,
/// before the runtime pins it for the host's call, as a guest thread could change the name meanwhile (ADR-0054 a3).
/// Null otherwise.
inline void (*place_checked)(int folder, const std::string &name) = nullptr;

/// Tests only: called with the same folder and name once the runtime has pinned the socket there, before the host's
/// call. Null otherwise.
inline void (*place_pinned)(int folder, const std::string &name) = nullptr;

/// Takes away the folders of abstract names in the folder `parent` that runtimes left whose processes are gone, with
/// their files and pins: the runtime's first use of its own does it in the user's temporary folder (sys_sockets.cpp).
void sweep_folders(int parent);

/// The runtime's own folder, the folder of abstract names, made at its first use; -1 when it cannot be made. It stays
/// open at the same descriptor for the process's life. memfd.h keeps its memfds' files there too.
int own_folder();

/// An unguessable name with `prefix` for one of the runtime's files in its own folder.
std::string own_file_name(std::string_view prefix);

/// Whether the app holds INTERNET (apkrt_set_internet, ADR-0054 step c2a): granted until the host says otherwise.
bool internet();
void set_internet(bool granted);

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
