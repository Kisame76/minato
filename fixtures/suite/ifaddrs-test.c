// ifaddrs-test: the interfaces an app sees (ADR-0054 step c): getifaddrs' addresses of the loopback interface, its
// index and name both ways, its flags, MTU, index and name through the ifreq ioctls, the index on an IPv6 socket too,
// and the answers for a name and an index that are no interface. Only lo's lines: every other interface differs from
// device to device. On the emulator the fixture runs as the shell, whose getifaddrs takes the link dump an app's does
// without (bionic's ifaddrs.cpp), so the lines take the addresses alone, and the flags' low 16 bits, which both have.
// No AF_UNIX socket: SELinux lets an app use fewer ioctls on one than the shell. Then a NETLINK_ROUTE socket of its
// own: an IPv4 address dump's lo, the answer's sender and port, NLMSG_DONE, and the kernel's checks of names,
// types and options, which are the same for the shell and an app (bind, RTM_GETLINK and ioctls are not).
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *error_name(int error) {
    switch (error) {
        case ENODEV: return "ENODEV";
        case ENXIO: return "ENXIO";
        case EINVAL: return "EINVAL";
        case EFAULT: return "EFAULT";
        case EPERM: return "EPERM";
        case EACCES: return "EACCES";
        case ENOTTY: return "ENOTTY";
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
        case EOPNOTSUPP: return "EOPNOTSUPP";
        case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
        default: {
            static char other[24];
            snprintf(other, sizeof other, "errno %d", error);
            return other;
        }
    }
}

static const void *bytes_of(const struct sockaddr *address) {
    return address->sa_family == AF_INET ? (const void *)&((const struct sockaddr_in *)address)->sin_addr
                                         : (const void *)&((const struct sockaddr_in6 *)address)->sin6_addr;
}

// An ifreq ioctl of `name` on `fd`: the call's answer, or -1 and errno's name.
static int ask(int fd, unsigned long request, struct ifreq *ifr, const char *label) {
    const int result = ioctl(fd, request, ifr);
    if (result < 0) printf("%s: -1 %s\n", label, error_name(errno));
    return result;
}

// The IPv4 getters of an AF_INET socket on lo, an alias's label, and SIOCGIFCONF's first entry.
static void inet_getters(int fd) {
    const unsigned long requests[] = {SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFDSTADDR};
    const char *names[] = {"SIOCGIFADDR", "SIOCGIFNETMASK", "SIOCGIFBRDADDR", "SIOCGIFDSTADDR"};
    for (int each = 0; each < 4; ++each) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strcpy(ifr.ifr_name, "lo");
        char label[48];
        snprintf(label, sizeof label, "%s lo", names[each]);
        if (ask(fd, requests[each], &ifr, label) != 0) continue;
        const struct sockaddr_in *address = (const struct sockaddr_in *)&ifr.ifr_addr;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address->sin_addr, text, sizeof text);
        printf("%s: family %d, %s\n", label, address->sin_family, text);
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strcpy(ifr.ifr_name, "lo:9");
    if (ask(fd, SIOCGIFINDEX, &ifr, "SIOCGIFINDEX lo:9") == 0) printf("SIOCGIFINDEX lo:9: %d\n", ifr.ifr_ifindex);
    memset(&ifr, 0, sizeof ifr);
    strcpy(ifr.ifr_name, "lo:9");
    ask(fd, SIOCGIFADDR, &ifr, "SIOCGIFADDR lo:9");
    struct ifconf conf;
    memset(&conf, 0, sizeof conf);
    if (ask(fd, SIOCGIFCONF, (struct ifreq *)&conf, "SIOCGIFCONF without a buffer") == 0) {
        printf("SIOCGIFCONF without a buffer: whole entries %s\n",
               conf.ifc_len > 0 && conf.ifc_len % (int)sizeof(struct ifreq) == 0 ? "yes" : "no");
    }
    struct ifreq first[1];
    memset(first, 0, sizeof first);
    conf.ifc_len = sizeof first;
    conf.ifc_req = first;
    if (ask(fd, SIOCGIFCONF, (struct ifreq *)&conf, "SIOCGIFCONF") == 0) {
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((const struct sockaddr_in *)&first[0].ifr_addr)->sin_addr, text, sizeof text);
        printf("SIOCGIFCONF: length %d, %s %s\n", conf.ifc_len, first[0].ifr_name, text);
    }
}

// A NETLINK_ROUTE socket of the fixture's own, as libraries use one.
static void netlink_part(void) {
    printf("netlink stream: %s\n", socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE) == -1 ? error_name(errno) : "made");
    int pair[2];
    printf("netlink socketpair: %s\n", socketpair(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE, pair) == -1 ? error_name(errno)
                                                                                                    : "made");
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct sockaddr_nl own;
    socklen_t length = sizeof own;
    memset(&own, 0, sizeof own);
    getsockname(fd, (struct sockaddr *)&own, &length);
    printf("netlink before a send: length %u, family %d, port %u\n", length, own.nl_family, own.nl_pid);
    struct sockaddr_nl other;
    memset(&other, 0, sizeof other);
    other.nl_family = AF_NETLINK;
    other.nl_pid = 1234;
    printf("connect to port 1234: %s\n", connect(fd, (struct sockaddr *)&other, sizeof other) == -1 ? error_name(errno)
                                                                                                  : "connected");
    struct {
        struct nlmsghdr header;
        struct rtgenmsg message;
    } request;
    memset(&request, 0, sizeof request);
    request.header.nlmsg_len = sizeof request;
    request.header.nlmsg_type = RTM_GETADDR;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 77;
    request.message.rtgen_family = AF_INET;
    other.nl_pid = 5;
    printf("sendto port 5: %s\n", sendto(fd, &request, sizeof request, 0, (struct sockaddr *)&other, sizeof other) == -1
                                       ? error_name(errno) : "sent");
    printf("send: %zd\n", send(fd, &request, sizeof request, 0));
    length = sizeof own;
    getsockname(fd, (struct sockaddr *)&own, &length);
    printf("netlink port is the process ID: %s\n", own.nl_pid == (unsigned)getpid() ? "yes" : "no");
    char small[16];
    printf("first datagram longer than 16 bytes: %s\n",
           recv(fd, small, sizeof small, MSG_PEEK | MSG_TRUNC) > 16 ? "yes" : "no");
    printf("FIONREAD: %s\n", ioctl(fd, FIONREAD, &length) == -1 ? error_name(errno) : "answered");
    char buffer[16384];
    int lo = 0, done = 0, pids = 1;
    while (!done) {
        struct sockaddr_nl from;
        socklen_t from_length = sizeof from;
        const ssize_t got = recvfrom(fd, buffer, sizeof buffer, 0, (struct sockaddr *)&from, &from_length);
        if (got <= 0) break;
        if (from_length != sizeof from || from.nl_family != AF_NETLINK || from.nl_pid != 0 || from.nl_groups != 0) {
            printf("sender: not the kernel\n");
        }
        size_t left = (size_t)got;
        for (struct nlmsghdr *header = (struct nlmsghdr *)buffer; NLMSG_OK(header, left);
             header = NLMSG_NEXT(header, left)) {
            if (header->nlmsg_pid != own.nl_pid || header->nlmsg_seq != 77) pids = 0;
            if (header->nlmsg_type == NLMSG_DONE) {
                printf("NLMSG_DONE: length %u, flags 0x%x, error %d\n", header->nlmsg_len, header->nlmsg_flags,
                       *(int *)NLMSG_DATA(header));
                done = 1;
                break;
            }
            const struct ifaddrmsg *message = NLMSG_DATA(header);
            if (header->nlmsg_type != RTM_NEWADDR || message->ifa_index != 1) continue;
            char label[IFNAMSIZ] = "", local[INET_ADDRSTRLEN] = "";
            size_t room = IFA_PAYLOAD(header);
            for (struct rtattr *each = IFA_RTA(message); RTA_OK(each, room); each = RTA_NEXT(each, room)) {
                if (each->rta_type == IFA_LABEL) snprintf(label, sizeof label, "%s", (const char *)RTA_DATA(each));
                if (each->rta_type == IFA_LOCAL) inet_ntop(AF_INET, RTA_DATA(each), local, sizeof local);
            }
            printf("lo in the dump: %s/%d, scope %d, label %s, flags 0x%x, multi %s\n", local, message->ifa_prefixlen,
                   message->ifa_scope, label, message->ifa_flags, (header->nlmsg_flags & NLM_F_MULTI) ? "yes" : "no");
            ++lo;
        }
    }
    printf("lo addresses: %d, each answer with the request's sequence and the socket's port: %s\n", lo,
           pids ? "yes" : "no");
    int group = RTNLGRP_IPV4_IFADDR;
    printf("join RTNLGRP_IPV4_IFADDR: %d\n", setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof group));
    unsigned int words[4] = {0};
    socklen_t words_length = sizeof words;
    if (getsockopt(fd, SOL_NETLINK, NETLINK_LIST_MEMBERSHIPS, words, &words_length) == 0) {
        printf("memberships: length %u, 0x%x 0x%x\n", words_length, words[0], words[1]);
    }
    length = sizeof own;
    getsockname(fd, (struct sockaddr *)&own, &length);
    printf("getsockname's groups: 0x%x\n", own.nl_groups);
    group = 37;
    const int joined = setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof group);
    printf("join group 37: %s\n", joined == -1 ? error_name(errno) : "joined");
    group = RTNLGRP_IPV4_MROUTE_R;
    const int mroute = setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof group);
    printf("join RTNLGRP_IPV4_MROUTE_R: %s\n", mroute == -1 ? error_name(errno) : "joined");
    int on = 1;
    printf("NETLINK_LISTEN_ALL_NSID: %s\n",
           setsockopt(fd, SOL_NETLINK, NETLINK_LISTEN_ALL_NSID, &on, sizeof on) == -1 ? error_name(errno) : "set");
    printf("shutdown: %s\n", shutdown(fd, SHUT_RDWR) == -1 ? error_name(errno) : "done");
    close(fd);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct ifaddrs *list = NULL;
    printf("getifaddrs: %d\n", getifaddrs(&list));
    for (const struct ifaddrs *each = list; each != NULL; each = each->ifa_next) {
        if (strcmp(each->ifa_name, "lo") != 0 || each->ifa_addr == NULL) continue;
        const int family = each->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        char address[INET6_ADDRSTRLEN] = "", mask[INET6_ADDRSTRLEN] = "none";
        inet_ntop(family, bytes_of(each->ifa_addr), address, sizeof address);
        if (each->ifa_netmask != NULL) inet_ntop(family, bytes_of(each->ifa_netmask), mask, sizeof mask);
        printf("lo %s: %s, netmask %s, flags 0x%x\n", family == AF_INET ? "inet" : "inet6", address, mask,
               each->ifa_flags & 0xffff);
    }
    freeifaddrs(list);

    char name[IF_NAMESIZE] = "";
    printf("if_nametoindex lo: %u\n", if_nametoindex("lo"));
    printf("if_indextoname 1: %s\n", if_indextoname(1, name) != NULL ? name : "null");
    printf("if_nametoindex of none: %u\n", if_nametoindex("minato0"));
    errno = 0;
    printf("if_indextoname of 999: %s\n", if_indextoname(999, name) != NULL ? name : error_name(errno));

    const int families[] = {AF_INET, AF_INET6};
    const char *names[] = {"inet", "inet6"};
    for (int index = 0; index < 2; ++index) {
        const int fd = socket(families[index], SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strcpy(ifr.ifr_name, "lo");
        char label[64];
        snprintf(label, sizeof label, "%s socket: SIOCGIFINDEX lo", names[index]);
        if (ask(fd, SIOCGIFINDEX, &ifr, label) == 0) printf("%s: %d\n", label, ifr.ifr_ifindex);
        if (index == 0) {
            memset(&ifr, 0, sizeof ifr);
            strcpy(ifr.ifr_name, "lo");
            if (ask(fd, SIOCGIFFLAGS, &ifr, "SIOCGIFFLAGS lo") == 0) {
                printf("SIOCGIFFLAGS lo: 0x%x\n", ifr.ifr_flags & 0xffff);
            }
            memset(&ifr, 0, sizeof ifr);
            strcpy(ifr.ifr_name, "lo");
            if (ask(fd, SIOCGIFMTU, &ifr, "SIOCGIFMTU lo") == 0) printf("SIOCGIFMTU lo: %d\n", ifr.ifr_mtu);
            memset(&ifr, 0, sizeof ifr);
            ifr.ifr_ifindex = 1;
            if (ask(fd, SIOCGIFNAME, &ifr, "SIOCGIFNAME 1") == 0) printf("SIOCGIFNAME 1: %s\n", ifr.ifr_name);
            memset(&ifr, 0, sizeof ifr);
            strcpy(ifr.ifr_name, "minato0");
            ask(fd, SIOCGIFFLAGS, &ifr, "SIOCGIFFLAGS of none");
            ask(fd, SIOCGIFINDEX, &ifr, "SIOCGIFINDEX of none");
            memset(&ifr, 0, sizeof ifr);
            ifr.ifr_ifindex = 999;
            ask(fd, SIOCGIFNAME, &ifr, "SIOCGIFNAME 999");
            inet_getters(fd);
        } else {
            memset(&ifr, 0, sizeof ifr);
            strcpy(ifr.ifr_name, "lo");
            ask(fd, SIOCGIFADDR, &ifr, "inet6 socket: SIOCGIFADDR lo");
        }
        close(fd);
    }
    netlink_part();
    return 0;
}
