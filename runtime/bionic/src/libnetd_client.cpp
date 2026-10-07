// Minato's libnetd_client.so (ADR-0021, ADR-0054 step b). bionic's libc.so opens it in every process at start
// (NetdClient.cpp) and passes each socket function to one of these init functions, which on Android wrap them so
// that the netd service can mark and route the sockets. Here, as Android 15's own (system/netd's client/NetdClient.cpp,
// read, not copied): the socket function refuses AF_INET and AF_INET6 while the process may not use the network
// (setAllowNetworkingForProcess, which Zygote calls for an app without INTERNET), and bionic's resolver opens the proxy
// at /dev/socket/dnsproxyd, which the runtime serves. There is no fwmarkd: the other functions keep bionic's own. The
// library exists in every process, so that the dlopen succeeds as on Android and leaves no error behind for the
// program's first dlerror().

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

typedef int (*SocketFunction)(int, int, int);
typedef int (*DnsOpenProxyFunction)();

bool allow_networking = true;           // read and written atomically
SocketFunction libc_socket = nullptr;   // bionic's socket, set once at start

bool networking_allowed() {
    return __atomic_load_n(&allow_networking, __ATOMIC_SEQ_CST);
}

int netd_client_socket(int domain, int type, int protocol) {
    if ((domain == AF_INET || domain == AF_INET6) && !networking_allowed()) {
        errno = EPERM;
        return -1;
    }
    return libc_socket(domain, type, protocol);
}

/// A connection to the resolver's proxy, or -1 with errno: ENOSYS when ANDROID_DNS_MODE is "local", EPERM without
/// networking (bionic then resolves on its own, from /system/etc/hosts), or connect's error.
int dns_open_proxy() {
    const char *mode = getenv("ANDROID_DNS_MODE");
    if (mode != nullptr && strcmp(mode, "local") == 0) {
        errno = ENOSYS;
        return -1;
    }
    if (!networking_allowed()) {
        errno = EPERM;
        return -1;
    }
    const int fd = (libc_socket != nullptr ? libc_socket : socket)(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_un proxy;
    memset(&proxy, 0, sizeof proxy);
    proxy.sun_family = AF_UNIX;
    strcpy(proxy.sun_path, "/dev/socket/dnsproxyd");
    int result;
    do {
        result = connect(fd, reinterpret_cast<const struct sockaddr *>(&proxy), sizeof proxy);
    } while (result == -1 && errno == EINTR);
    if (result != 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

}  // namespace

extern "C" {

#define APKRT_KEEP_BIONIC_FUNCTION(name) \
  __attribute__((visibility("default"))) void name(void*) {}

APKRT_KEEP_BIONIC_FUNCTION(netdClientInitAccept4)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitConnect)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendmmsg)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendmsg)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendto)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitNetIdForResolv)

__attribute__((visibility("default"))) void netdClientInitSocket(SocketFunction *function) {
    if (function != nullptr && *function != nullptr) {
        libc_socket = *function;
        *function = netd_client_socket;
    }
}

__attribute__((visibility("default"))) void netdClientInitDnsOpenProxy(DnsOpenProxyFunction *function) {
    if (function != nullptr) *function = dns_open_proxy;
}

__attribute__((visibility("default"))) void setAllowNetworkingForProcess(bool allow) {
    __atomic_store_n(&allow_networking, allow, __ATOMIC_SEQ_CST);
}

}  // extern "C"
