// The guest's network interfaces and its NETLINK_ROUTE sockets (ADR-0054 step c): the interfaces an app sees on
// Android 15, made from the host's, and what Android 15's kernel answers an app's requests, SELinux's refusals
// included (system/sepolicy's app_neverallows.te and net.te). sys_sockets.cpp keeps the sockets, sys_files.cpp asks
// the ioctls here.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace apkrt::netlink {

/// An address of an interface: Linux's family (2 or 10), its 4 or 16 bytes, its prefix length, and an IPv4
/// interface's broadcast address or a point-to-point interface's peer, empty for none.
struct Address {
    int family = 0;
    std::string bytes;
    int prefix = 0;
    std::string broadcast, peer;
};

/// An interface as the host's getifaddrs has it: its name, index, Darwin's flags, MTU and addresses, a link-local
/// IPv6 address without a scope in its bytes 2 and 3, as getifaddrs gives it.
struct HostInterface {
    std::string name;
    unsigned index = 0;
    unsigned flags = 0;
    int mtu = 0;
    std::vector<Address> addresses;
};

/// An interface as the guest sees it: Linux's name, the host's index, Linux's flags and the MTU, and its addresses.
struct Interface {
    std::string name, host_name;
    unsigned index = 0;
    unsigned flags = 0;
    int mtu = 0;
    std::vector<Address> addresses;
};

/// The guest's interfaces of the host's, in the order of their indexes, the host's: lo0 as Linux's lo, index 1,
/// with 127.0.0.1/8 and ::1/128 alone, UP, LOOPBACK and RUNNING, and an MTU of 65536; en0 as wlan0; every pdp_ipN
/// with an address as rmnet_dataN; each utun with an address that is not link-local as tunN, and each other en
/// with one as ethN, numbered from 0 in their order. The rest is hidden, as no Android device has it.
std::vector<Interface> guest_interfaces(const std::vector<HostInterface> &host);

/// The host's interfaces now.
std::vector<HostInterface> host_interfaces();

/// The guest's interfaces now: guest_interfaces of host_interfaces.
std::vector<Interface> interfaces();

/// Linux's NETLINK_ROUTE (0), the only netlink protocol an app may open on Android 15.
constexpr int netlink_route = 0;

/// Linux's sockaddr_nl of the kernel, the sender of every answer: AF_NETLINK, port 0, no groups.
std::string kernel_name();

/// The kernel's answer to the datagram `request` that an app sent on a NETLINK_ROUTE socket whose port is `port`:
/// the datagrams it receives, appended to `replies`, and 0, or a negative Linux errno for the send, with no
/// datagram: EACCES for a message SELinux refuses an app (any but a request to read, RTM_GETLINK, RTM_GETNEIGH and
/// RTM_GETNEIGHTBL). An RTM_GETADDR dump answers `interfaces`' addresses, asked for only then.
long answer(std::string_view request, std::uint32_t port, const std::function<std::vector<Interface>()> &interfaces,
            std::vector<std::string> &replies);

/// Linux's ifreq ioctls on an AF_INET or AF_INET6 socket of the guest (`inet` for AF_INET) at `argument`: SIOCGIFNAME,
/// SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFMTU and SIOCGIFCONF on both, and SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFBRDADDR
/// and SIOCGIFDSTADDR on AF_INET; ENODEV for a name or index no interface has. `handled` is false for any other
/// request, which this leaves alone.
long interface_ioctl(std::uint32_t request, std::uint64_t argument, bool inet, bool &handled);

/// Whether `request` is one of Linux's ifreq ioctls above, or SIOCGIFHWADDR, which SELinux refuses an app on every
/// socket, as it refuses all of them on an AF_UNIX socket.
bool is_interface_ioctl(std::uint32_t request);

}  // namespace apkrt::netlink
