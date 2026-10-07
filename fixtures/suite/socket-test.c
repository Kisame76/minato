// socket-test: sockets as apps and libcore use them (ADR-0054 step a): socket's checks and flags, socketpairs,
// SCM_RIGHTS and SO_PEERCRED, AF_UNIX's abstract names and its errors for names in the file system, TCP and UDP on
// 127.0.0.1 and ::1 with v4-mapped addresses, the MSG flags, timeouts, EPIPE with SIGPIPE, options read back,
// sendmmsg and recvmmsg, epoll on sockets, and a blocked call that a signal interrupts. Nothing it prints depends
// on a port, an address or a process ID. Expected output: fixtures/expected/socket-test.out
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void report(const char *what, long result) {
    if (result == -1) {
        printf("%s: -1 errno %d (%s)\n", what, errno, strerror(errno));
    } else {
        printf("%s: %ld\n", what, result);
    }
}

static long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
    struct timespec time = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&time, NULL);
}

static int int_option(int fd, int level, int name) {
    int value = -1;
    socklen_t length = sizeof value;
    if (getsockopt(fd, level, name, &value, &length) != 0) return -1000 - errno;
    return length == sizeof value ? value : -2000 - (int)length;
}

static volatile sig_atomic_t sigpipes, sigusr1s;

static void on_sigpipe(int signal) {
    (void)signal;
    ++sigpipes;
}

static void on_sigusr1(int signal) {
    (void)signal;
    ++sigusr1s;
}

static void install(int signal, void (*handler)(int), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigaction(signal, &action, NULL);
}

// --- socket ------------------------------------------------------------------------------------------------

static void test_socket(void) {
    printf("-- socket\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("AF_INET stream: %d\n", fd >= 0);
    close(fd);
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    printf("AF_INET6 datagram: %d\n", fd >= 0);
    close(fd);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    printf("AF_UNIX with flags: FD_CLOEXEC %d, O_NONBLOCK %d\n", (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
           (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    printf("  SO_TYPE %d, SO_DOMAIN %d, SO_PROTOCOL %d, SO_ACCEPTCONN %d\n", int_option(fd, SOL_SOCKET, SO_TYPE),
           int_option(fd, SOL_SOCKET, SO_DOMAIN), int_option(fd, SOL_SOCKET, SO_PROTOCOL),
           int_option(fd, SOL_SOCKET, SO_ACCEPTCONN));
    struct stat status;
    printf("  fstat is a socket: %d\n", fstat(fd, &status) == 0 && S_ISSOCK(status.st_mode));
    close(fd);
    report("an unknown family", socket(12345, SOCK_STREAM, 0));
    report("an unknown type", socket(AF_INET, 99, 0));
    report("UDP as a stream", socket(AF_INET, SOCK_STREAM, IPPROTO_UDP));
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    printf("TCP by number: SO_PROTOCOL %d\n", int_option(fd, SOL_SOCKET, SO_PROTOCOL));
    close(fd);
    int file = open("/data/local/tmp", O_RDONLY | O_DIRECTORY);
    report("getsockopt of a directory", int_option(file, SOL_SOCKET, SO_TYPE) == -1000 - ENOTSOCK ? 0 : -1);
    close(file);
}

// --- socketpair --------------------------------------------------------------------------------------------

static void test_socketpair_stream(void) {
    printf("-- socketpair, stream\n");
    int pair[2];
    report("socketpair", socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    printf("FD_CLOEXEC on both: %d\n", (fcntl(pair[0], F_GETFD) & FD_CLOEXEC) && (fcntl(pair[1], F_GETFD) & FD_CLOEXEC));
    report("write", write(pair[0], "ping", 4));
    char buffer[16] = {0};
    report("read", read(pair[1], buffer, sizeof buffer));
    printf("  read %s\n", buffer);
    report("send with MSG_DONTWAIT", send(pair[1], "pong", 4, MSG_DONTWAIT));
    report("recv with MSG_PEEK", recv(pair[0], buffer, 2, MSG_PEEK));
    report("recv", recv(pair[0], buffer, sizeof buffer, 0));
    report("recv of nothing with MSG_DONTWAIT", recv(pair[0], buffer, sizeof buffer, MSG_DONTWAIT));
    struct sockaddr_un name;
    socklen_t length = sizeof name;
    memset(&name, 0x55, sizeof name);
    report("getsockname of an unnamed socket", getsockname(pair[0], (struct sockaddr *)&name, &length));
    printf("  length %d, family %d\n", (int)length, name.sun_family);
    length = sizeof name;
    report("getpeername", getpeername(pair[0], (struct sockaddr *)&name, &length));
    printf("  length %d\n", (int)length);
    struct ucred credentials;
    length = sizeof credentials;
    report("SO_PEERCRED", getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &credentials, &length));
    printf("  length %d, the peer is this process: %d\n", (int)length,
           credentials.pid == getpid() && credentials.uid == getuid() && credentials.gid == getgid());
    report("shutdown SHUT_WR", shutdown(pair[1], SHUT_WR));
    report("read after the peer's shutdown", read(pair[0], buffer, sizeof buffer));
    int folder = open("/data/local/tmp", O_RDONLY | O_DIRECTORY);
    report("shutdown of a folder", shutdown(folder, SHUT_RD));
    close(folder);
    // EPIPE: SIGPIPE along with it, unless MSG_NOSIGNAL.
    install(SIGPIPE, on_sigpipe, 0);
    close(pair[1]);
    report("send to a closed peer", send(pair[0], "x", 1, 0));
    printf("  SIGPIPE %d\n", (int)sigpipes);
    report("send with MSG_NOSIGNAL", send(pair[0], "x", 1, MSG_NOSIGNAL));
    printf("  SIGPIPE %d\n", (int)sigpipes);
    report("write to a closed peer", write(pair[0], "x", 1));
    printf("  SIGPIPE %d\n", (int)sigpipes);
    close(pair[0]);
    install(SIGPIPE, SIG_DFL, 0);
}

static void test_socketpair_datagram(void) {
    printf("-- socketpair, datagram\n");
    int pair[2];
    report("socketpair", socketpair(AF_UNIX, SOCK_DGRAM, 0, pair));
    char big[5000];
    memset(big, 'd', sizeof big);
    report("send 1", send(pair[0], big, 1, 0));
    report("send 100", send(pair[0], big, 100, 0));
    report("send 5000", send(pair[0], big, sizeof big, 0));
    char buffer[6000];
    report("recv", recv(pair[1], buffer, sizeof buffer, 0));
    report("recv of 100 into 10 bytes", recv(pair[1], buffer, 10, 0));
    report("recv of 5000 into 10 bytes with MSG_TRUNC", recv(pair[1], buffer, 10, MSG_TRUNC));
    report("send 100", send(pair[0], big, 100, 0));
    struct iovec vector = {buffer, 10};
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    report("recvmsg into 10 bytes", recvmsg(pair[1], &message, 0));
    printf("  MSG_TRUNC %d, MSG_CTRUNC %d\n", (message.msg_flags & MSG_TRUNC) != 0, (message.msg_flags & MSG_CTRUNC) != 0);
    report("SO_TYPE", int_option(pair[0], SOL_SOCKET, SO_TYPE));
    close(pair[0]);
    close(pair[1]);
}

static void print_poll(const char *what, int fd) {
    struct pollfd entry = {fd, POLLIN | POLLOUT | POLLRDHUP, 0};
    int ready = poll(&entry, 1, 0);
    printf("%s: %d, POLLIN %d, POLLOUT %d, POLLRDHUP %d, POLLHUP %d, POLLERR %d\n", what, ready,
           (entry.revents & POLLIN) != 0, (entry.revents & POLLOUT) != 0, (entry.revents & POLLRDHUP) != 0,
           (entry.revents & POLLHUP) != 0, (entry.revents & POLLERR) != 0);
}

static void test_socketpair_seqpacket(void) {
    printf("-- socketpair, seqpacket\n");
    int pair[2];
    report("socketpair", socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    printf("  SO_TYPE %d, FD_CLOEXEC %d\n", int_option(pair[0], SOL_SOCKET, SO_TYPE),
           (fcntl(pair[0], F_GETFD) & FD_CLOEXEC) != 0);
    print_poll("poll at the start", pair[1]);
    char big[5000];
    memset(big, 's', sizeof big);
    report("send 1", send(pair[0], big, 1, 0));
    report("send 100", send(pair[0], big, 100, 0));
    report("send 0", send(pair[0], big, 0, 0));
    report("write 5000", write(pair[0], big, sizeof big));
    char buffer[6000];
    report("recv with MSG_PEEK", recv(pair[1], buffer, sizeof buffer, MSG_PEEK));
    report("recv", recv(pair[1], buffer, sizeof buffer, 0));
    report("recv of 100 into 10 bytes", recv(pair[1], buffer, 10, 0));
    report("recv of the empty one", recv(pair[1], buffer, sizeof buffer, 0));
    struct iovec vector = {buffer, 10};
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    report("recvmsg of 5000 into 10 bytes with MSG_TRUNC", recvmsg(pair[1], &message, MSG_TRUNC));
    printf("  MSG_TRUNC %d, MSG_EOR %d\n", (message.msg_flags & MSG_TRUNC) != 0, (message.msg_flags & MSG_EOR) != 0);
    report("recv of nothing with MSG_DONTWAIT", recv(pair[1], buffer, sizeof buffer, MSG_DONTWAIT));
    report("write back", write(pair[1], "abc", 3));
    report("read of a message", read(pair[0], buffer, sizeof buffer));
    struct sockaddr_un name;
    socklen_t length = sizeof name;
    report("getpeername", getpeername(pair[0], (struct sockaddr *)&name, &length));
    printf("  length %d\n", (int)length);
    // The longest message: the sender's buffer less 32 bytes, as a datagram's.
    int send_buffer = int_option(pair[0], SOL_SOCKET, SO_SNDBUF);
    printf("SO_SNDBUF %d, SO_RCVBUF %d\n", send_buffer, int_option(pair[0], SOL_SOCKET, SO_RCVBUF));
    char *huge = calloc(1, (size_t)send_buffer);
    report("send of SO_SNDBUF less 32 bytes", send(pair[0], huge, (size_t)send_buffer - 32, MSG_DONTWAIT));
    report("recv of it", recv(pair[1], huge, (size_t)send_buffer, 0));
    report("send of one byte more", send(pair[0], huge, (size_t)send_buffer - 31, MSG_DONTWAIT));
    free(huge);
    // SHUT_WR: the peer reads the end, and still sends.
    report("shutdown SHUT_WR", shutdown(pair[0], SHUT_WR));
    print_poll("poll after the peer's SHUT_WR", pair[1]);
    report("recv after the peer's SHUT_WR", recv(pair[1], buffer, sizeof buffer, MSG_DONTWAIT));
    report("send after its own SHUT_WR", send(pair[0], "x", 1, MSG_NOSIGNAL));
    report("send to the side that shut down", send(pair[1], "y", 1, 0));
    report("recv there", recv(pair[0], buffer, sizeof buffer, 0));
    close(pair[0]);
    close(pair[1]);
    // close: what came before it, then the end; a send gets EPIPE.
    install(SIGPIPE, on_sigpipe, 0);
    sigpipes = 0;
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair);
    report("send before the close", send(pair[0], "last", 4, 0));
    close(pair[0]);
    print_poll("poll after the peer's close", pair[1]);
    report("recv of what came before the close", recv(pair[1], buffer, sizeof buffer, 0));
    report("recv after the peer's close", recv(pair[1], buffer, sizeof buffer, 0));
    report("recv again", recv(pair[1], buffer, sizeof buffer, MSG_DONTWAIT));
    report("send to a closed peer", send(pair[1], "x", 1, 0));
    printf("  SIGPIPE %d\n", (int)sigpipes);
    report("write to a closed peer", write(pair[1], "x", 1));
    printf("  SIGPIPE %d\n", (int)sigpipes);
    close(pair[1]);
    // A close with messages it never read: the peer's next call gets ECONNRESET, once.
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair);
    report("send to the side that closes", send(pair[1], "unread", 6, 0));
    close(pair[0]);
    print_poll("poll after a close with unread messages", pair[1]);
    report("recv with MSG_DONTWAIT after it", recv(pair[1], buffer, sizeof buffer, MSG_DONTWAIT));
    report("recv after it", recv(pair[1], buffer, sizeof buffer, 0));
    report("recv again", recv(pair[1], buffer, sizeof buffer, 0));
    close(pair[1]);
    install(SIGPIPE, SIG_DFL, 0);
}

static void test_rights(void) {
    printf("-- SCM_RIGHTS\n");
    int pair[2], pipe_ends[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    pipe(pipe_ends);
    write(pipe_ends[1], "through", 7);
    char data = 'r';
    struct iovec vector = {&data, 1};
    union {
        char buffer[CMSG_SPACE(2 * sizeof(int))];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = CMSG_SPACE(2 * sizeof(int));
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(header), pipe_ends, 2 * sizeof(int));
    report("sendmsg with two descriptors", sendmsg(pair[0], &message, 0));
    close(pipe_ends[0]);
    close(pipe_ends[1]);
    memset(&control, 0, sizeof control);
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof control.buffer;
    report("recvmsg with MSG_CMSG_CLOEXEC", recvmsg(pair[1], &message, MSG_CMSG_CLOEXEC));
    header = CMSG_FIRSTHDR(&message);
    printf("  controllen %d, level %d, type %d, len %d\n", (int)message.msg_controllen, header ? header->cmsg_level : -1,
           header ? header->cmsg_type : -1, header ? (int)header->cmsg_len : -1);
    int received[2] = {-1, -1};
    if (header) memcpy(received, CMSG_DATA(header), sizeof received);
    printf("  FD_CLOEXEC %d %d\n", (fcntl(received[0], F_GETFD) & FD_CLOEXEC) != 0,
           (fcntl(received[1], F_GETFD) & FD_CLOEXEC) != 0);
    char buffer[16] = {0};
    report("  read through the received pipe", read(received[0], buffer, sizeof buffer));
    printf("  read %s\n", buffer);
    report("  write to the received write end", write(received[1], "x", 1));
    close(received[0]);
    close(received[1]);
    // Room for the header only: the descriptors are dropped and MSG_CTRUNC tells.
    pipe(pipe_ends);
    message.msg_control = control.buffer;
    message.msg_controllen = CMSG_SPACE(sizeof(int));
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &pipe_ends[0], sizeof(int));
    message.msg_flags = 0;
    report("sendmsg with one descriptor", sendmsg(pair[0], &message, 0));
    close(pipe_ends[0]);
    close(pipe_ends[1]);
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = CMSG_LEN(0);
    report("recvmsg without room for it", recvmsg(pair[1], &message, 0));
    printf("  MSG_CTRUNC %d, controllen %d\n", (message.msg_flags & MSG_CTRUNC) != 0, (int)message.msg_controllen);
    close(pair[0]);
    close(pair[1]);
}

// --- AF_UNIX names -----------------------------------------------------------------------------------------

static socklen_t unix_name(struct sockaddr_un *name, const char *path, int abstract) {
    memset(name, 0, sizeof *name);
    name->sun_family = AF_UNIX;
    size_t length = strlen(path);
    if (abstract) {
        memcpy(name->sun_path + 1, path, length);
        return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
    }
    memcpy(name->sun_path, path, length);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
}

static void test_abstract_name(void) {
    printf("-- AF_UNIX, an abstract name\n");
    struct sockaddr_un name;
    socklen_t length = unix_name(&name, "socket-test-abstract", 1);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    report("bind", bind(server, (struct sockaddr *)&name, length));
    int other = socket(AF_UNIX, SOCK_STREAM, 0);
    report("bind again", bind(other, (struct sockaddr *)&name, length));
    close(other);
    report("listen", listen(server, 4));
    printf("  SO_ACCEPTCONN %d\n", int_option(server, SOL_SOCKET, SO_ACCEPTCONN));
    struct sockaddr_un got;
    socklen_t got_length = sizeof got;
    report("getsockname", getsockname(server, (struct sockaddr *)&got, &got_length));
    printf("  same name: %d\n", got_length == length && memcmp(&got, &name, length) == 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    report("connect", connect(client, (struct sockaddr *)&name, length));
    got_length = sizeof got;
    report("getpeername of the client", getpeername(client, (struct sockaddr *)&got, &got_length));
    printf("  the server's name: %d\n", got_length == length && memcmp(&got, &name, length) == 0);
    got_length = sizeof got;
    int accepted = accept4(server, (struct sockaddr *)&got, &got_length, SOCK_CLOEXEC);
    printf("accept4: %d, FD_CLOEXEC %d, peer name length %d\n", accepted >= 0,
           (fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0, (int)got_length);
    report("send", send(client, "named", 5, 0));
    char buffer[16] = {0};
    report("recv", recv(accepted, buffer, sizeof buffer, 0));
    close(accepted);
    close(client);
    close(server);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    report("connect after the server closed", connect(client, (struct sockaddr *)&name, length));
    close(client);
}

static void test_unix_names(void) {
    // Names in the file system are an app's (runtime.syscalls and net-test): Android's SELinux lets the shell,
    // which runs these fixtures there, create no socket in /data/local/tmp.
    test_abstract_name();
    printf("-- AF_UNIX, errors\n");
    struct sockaddr_un name;
    socklen_t length = unix_name(&name, "/data/local/tmp/socket-test-missing.sock", 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    report("connect to a missing file", connect(client, (struct sockaddr *)&name, length));
    length = unix_name(&name, "socket-test-nobody", 1);
    report("connect to an abstract name nobody has", connect(client, (struct sockaddr *)&name, length));
    close(client);
    int file = open("/data/local/tmp/socket-test-plain", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    close(file);
    length = unix_name(&name, "/data/local/tmp/socket-test-plain", 0);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    report("connect to a plain file", connect(client, (struct sockaddr *)&name, length));
    close(client);
    unlink("/data/local/tmp/socket-test-plain");
    // Autobind: a name of the family alone gets an abstract name of five hex digits.
    int server = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un family_only = {.sun_family = AF_UNIX};
    report("bind of the family alone", bind(server, (struct sockaddr *)&family_only, sizeof(sa_family_t)));
    struct sockaddr_un got;
    socklen_t got_length = sizeof got;
    getsockname(server, (struct sockaddr *)&got, &got_length);
    printf("  length %d, abstract %d\n", (int)got_length, got_length > sizeof(sa_family_t) && got.sun_path[0] == 0);
    close(server);
    // A datagram to a named socket: recvfrom names the sender.
    length = unix_name(&name, "socket-test-datagram", 1);
    server = socket(AF_UNIX, SOCK_DGRAM, 0);
    report("bind of a datagram socket", bind(server, (struct sockaddr *)&name, length));
    client = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un client_name;
    socklen_t client_length = unix_name(&client_name, "socket-test-datagram-client", 1);
    bind(client, (struct sockaddr *)&client_name, client_length);
    report("sendto", sendto(client, "dgram", 5, 0, (struct sockaddr *)&name, length));
    char buffer[16];
    got_length = sizeof got;
    report("recvfrom", recvfrom(server, buffer, sizeof buffer, 0, (struct sockaddr *)&got, &got_length));
    printf("  the sender's name: %d\n", got_length == client_length && memcmp(&got, &client_name, client_length) == 0);
    close(client);
    close(server);
}

// --- TCP ---------------------------------------------------------------------------------------------------

static socklen_t loopback(struct sockaddr_storage *name, int family, uint16_t port) {
    memset(name, 0, sizeof *name);
    if (family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)name;
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return sizeof *in;
    }
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)name;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(port);
    in6->sin6_addr = in6addr_loopback;
    return sizeof *in6;
}

static uint16_t port_of(int fd) {
    struct sockaddr_storage name;
    socklen_t length = sizeof name;
    if (getsockname(fd, (struct sockaddr *)&name, &length) != 0) return 0;
    return ntohs(name.ss_family == AF_INET ? ((struct sockaddr_in *)&name)->sin_port
                                           : ((struct sockaddr_in6 *)&name)->sin6_port);
}

static void tcp_round(int family) {
    printf("-- TCP over %s\n", family == AF_INET ? "127.0.0.1" : "::1");
    struct sockaddr_storage name;
    socklen_t length = loopback(&name, family, 0);
    int server = socket(family, SOCK_STREAM, 0);
    int on = 1;
    report("SO_REUSEADDR", setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on));
    report("bind to port 0", bind(server, (struct sockaddr *)&name, length));
    uint16_t port = port_of(server);
    printf("  a port: %d\n", port != 0);
    report("listen", listen(server, 8));
    printf("  SO_ACCEPTCONN %d, SO_REUSEADDR %d, SO_PROTOCOL %d, SO_DOMAIN is the family: %d\n",
           int_option(server, SOL_SOCKET, SO_ACCEPTCONN), int_option(server, SOL_SOCKET, SO_REUSEADDR),
           int_option(server, SOL_SOCKET, SO_PROTOCOL), int_option(server, SOL_SOCKET, SO_DOMAIN) == family);
    int client = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    length = loopback(&name, family, port);
    int connected = connect(client, (struct sockaddr *)&name, length);
    printf("non-blocking connect: 0 or EINPROGRESS: %d\n", connected == 0 || errno == EINPROGRESS);
    struct pollfd watched = {client, POLLOUT, 0};
    report("poll for POLLOUT", poll(&watched, 1, 5000));
    printf("  revents POLLOUT %d\n", (watched.revents & POLLOUT) != 0);
    printf("  SO_ERROR %d\n", int_option(client, SOL_SOCKET, SO_ERROR));
    report("connect again", connect(client, (struct sockaddr *)&name, length));
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof peer;
    int accepted = accept4(server, (struct sockaddr *)&peer, &peer_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    printf("accept4: %d, O_NONBLOCK %d, FD_CLOEXEC %d, the peer's family %d, length %d\n", accepted >= 0,
           (fcntl(accepted, F_GETFL) & O_NONBLOCK) != 0, (fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0,
           peer.ss_family == family, (int)peer_length);
    printf("  the peer's port is the client's: %d\n", port_of(client) == ntohs(family == AF_INET
                                                                                    ? ((struct sockaddr_in *)&peer)->sin_port
                                                                                    : ((struct sockaddr_in6 *)&peer)->sin6_port));
    peer_length = sizeof peer;
    getpeername(client, (struct sockaddr *)&peer, &peer_length);
    printf("  the client's peer is the server: %d\n", peer_length == length && memcmp(&peer, &name, length) == 0);
    // Blocking again: the flags are the guest's.
    fcntl(client, F_SETFL, fcntl(client, F_GETFL) & ~O_NONBLOCK);
    char buffer[32] = {0};
    report("recv of nothing with MSG_DONTWAIT", recv(client, buffer, sizeof buffer, MSG_DONTWAIT));
    report("send", send(accepted, "hello", 5, 0));
    report("recv with MSG_PEEK", recv(client, buffer, sizeof buffer, MSG_PEEK));
    report("recv", recv(client, buffer, sizeof buffer, 0));
    printf("  %s\n", buffer);
    report("send 5", send(accepted, "abcde", 5, 0));
    report("send 5 more", send(accepted, "fghij", 5, 0));
    memset(buffer, 0, sizeof buffer);
    report("recv of 10 with MSG_WAITALL", recv(client, buffer, 10, MSG_WAITALL));
    printf("  %s\n", buffer);
    struct timeval timeout = {0, 150000};
    report("SO_RCVTIMEO 150 ms", setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
    struct timeval got_timeout;
    socklen_t timeout_length = sizeof got_timeout;
    getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &got_timeout, &timeout_length);
    printf("  read back: %ld s %ld us, length %d\n", (long)got_timeout.tv_sec, (long)got_timeout.tv_usec,
           (int)timeout_length);
    long long start = now_ms();
    report("recv of nothing", recv(client, buffer, sizeof buffer, 0));
    printf("  waited at least 140 ms: %d\n", now_ms() - start >= 140);
    report("shutdown SHUT_WR", shutdown(accepted, SHUT_WR));
    report("recv at the end", recv(client, buffer, sizeof buffer, 0));
    close(accepted);
    close(client);
    close(server);
    // A port nobody listens on.
    server = socket(family, SOCK_STREAM, 0);
    length = loopback(&name, family, 0);
    bind(server, (struct sockaddr *)&name, length);
    port = port_of(server);
    close(server);
    client = socket(family, SOCK_STREAM, 0);
    length = loopback(&name, family, port);
    report("connect to a closed port", connect(client, (struct sockaddr *)&name, length));
    close(client);
}

static void test_options(void) {
    printf("-- options\n");
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int value = 1;
    report("TCP_NODELAY", setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, IPPROTO_TCP, TCP_NODELAY));
    report("SO_KEEPALIVE", setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, SOL_SOCKET, SO_KEEPALIVE));
    value = 60;
    report("TCP_KEEPIDLE 60", setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, IPPROTO_TCP, TCP_KEEPIDLE));
    value = 5;
    report("TCP_KEEPINTVL 5", setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, IPPROTO_TCP, TCP_KEEPINTVL));
    value = 3;
    report("TCP_KEEPCNT 3", setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, IPPROTO_TCP, TCP_KEEPCNT));
    struct linger linger = {1, 0};
    report("SO_LINGER on, 0 s", setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof linger));
    struct linger got = {-1, -1};
    socklen_t length = sizeof got;
    getsockopt(fd, SOL_SOCKET, SO_LINGER, &got, &length);
    printf("  read back on %d, %d s, length %d\n", got.l_onoff, got.l_linger, (int)length);
    value = 65536;
    report("SO_RCVBUF 65536", setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, SOL_SOCKET, SO_RCVBUF));
    value = 32768;
    report("SO_SNDBUF 32768", setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, SOL_SOCKET, SO_SNDBUF));
    // The buffers before SO_SNDBUF and SO_RCVBUF set them, and the floor and ceiling of what they set.
    static const struct {
        const char *name;
        int family, type;
    } kinds[] = {{"TCP", AF_INET, SOCK_STREAM},
                 {"UDP", AF_INET, SOCK_DGRAM},
                 {"AF_UNIX stream", AF_UNIX, SOCK_STREAM},
                 {"AF_UNIX datagram", AF_UNIX, SOCK_DGRAM}};
    for (int kind = 0; kind < (int)(sizeof kinds / sizeof kinds[0]); ++kind) {
        int fresh = socket(kinds[kind].family, kinds[kind].type, 0);
        printf("%s at first: SO_SNDBUF %d, SO_RCVBUF %d\n", kinds[kind].name, int_option(fresh, SOL_SOCKET, SO_SNDBUF),
               int_option(fresh, SOL_SOCKET, SO_RCVBUF));
        close(fresh);
    }
    int sized = socket(AF_UNIX, SOCK_DGRAM, 0);
    int sizes[] = {1, 1 << 30};
    for (int size = 0; size < 2; ++size) {
        setsockopt(sized, SOL_SOCKET, SO_SNDBUF, &sizes[size], sizeof sizes[size]);
        setsockopt(sized, SOL_SOCKET, SO_RCVBUF, &sizes[size], sizeof sizes[size]);
        printf("SO_SNDBUF and SO_RCVBUF %d: read back %d and %d\n", sizes[size], int_option(sized, SOL_SOCKET, SO_SNDBUF),
               int_option(sized, SOL_SOCKET, SO_RCVBUF));
    }
    close(sized);
    printf("IPV6_V6ONLY %d\n", int_option(fd, IPPROTO_IPV6, IPV6_V6ONLY));
    value = 0x20;
    report("IPV6_TCLASS 0x20", setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &value, sizeof value));
    printf("  read back %#x\n", int_option(fd, IPPROTO_IPV6, IPV6_TCLASS));
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    value = 0x10;
    report("IP_TOS 0x10", setsockopt(fd, IPPROTO_IP, IP_TOS, &value, sizeof value));
    printf("  read back %#x\n", int_option(fd, IPPROTO_IP, IP_TOS));
    value = 64;
    report("IP_TTL 64", setsockopt(fd, IPPROTO_IP, IP_TTL, &value, sizeof value));
    printf("  read back %d\n", int_option(fd, IPPROTO_IP, IP_TTL));
    report("an unknown level", setsockopt(fd, 12345, 1, &value, sizeof value));
    report("an unknown option", setsockopt(fd, SOL_SOCKET, 12345, &value, sizeof value));
    report("an option too short", setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, 1));
    close(fd);
}

static void test_mapped(void) {
    printf("-- v4-mapped\n");
    struct sockaddr_storage name;
    socklen_t length = loopback(&name, AF_INET, 0);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    bind(server, (struct sockaddr *)&name, length);
    listen(server, 1);
    struct sockaddr_in6 mapped;
    memset(&mapped, 0, sizeof mapped);
    mapped.sin6_family = AF_INET6;
    mapped.sin6_port = htons(port_of(server));
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &mapped.sin6_addr);
    int client = socket(AF_INET6, SOCK_STREAM, 0);
    report("connect of an AF_INET6 socket to ::ffff:127.0.0.1", connect(client, (struct sockaddr *)&mapped, sizeof mapped));
    struct sockaddr_in6 peer;
    socklen_t peer_length = sizeof peer;
    getpeername(client, (struct sockaddr *)&peer, &peer_length);
    printf("  the peer is v4-mapped: %d, length %d\n", IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr), (int)peer_length);
    struct sockaddr_in accepted_peer;
    socklen_t accepted_length = sizeof accepted_peer;
    int accepted = accept(server, (struct sockaddr *)&accepted_peer, &accepted_length);
    printf("  accepted as AF_INET: %d, from 127.0.0.1: %d\n", accepted_peer.sin_family == AF_INET,
           accepted_peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(accepted);
    close(client);
    close(server);
}

// --- UDP ---------------------------------------------------------------------------------------------------

static void udp_round(int family) {
    printf("-- UDP over %s\n", family == AF_INET ? "127.0.0.1" : "::1");
    struct sockaddr_storage name;
    socklen_t length = loopback(&name, family, 0);
    int receiver = socket(family, SOCK_DGRAM, 0);
    report("bind", bind(receiver, (struct sockaddr *)&name, length));
    int on = 1;
    if (family == AF_INET) {
        report("IP_PKTINFO", setsockopt(receiver, IPPROTO_IP, IP_PKTINFO, &on, sizeof on));
    } else {
        report("IPV6_RECVPKTINFO", setsockopt(receiver, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof on));
    }
    length = loopback(&name, family, port_of(receiver));
    int sender = socket(family, SOCK_DGRAM, 0);
    report("sendto", sendto(sender, "datagram", 8, 0, (struct sockaddr *)&name, length));
    char buffer[64] = {0};
    struct sockaddr_storage from;
    struct iovec vector = {buffer, sizeof buffer};
    union {
        char buffer[256];
        struct cmsghdr align;
    } control;
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_name = &from;
    message.msg_namelen = sizeof from;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof control.buffer;
    report("recvmsg", recvmsg(receiver, &message, 0));
    printf("  %s, from the sender's port: %d, name length %d\n", buffer,
           ntohs(family == AF_INET ? ((struct sockaddr_in *)&from)->sin_port
                                   : ((struct sockaddr_in6 *)&from)->sin6_port) == port_of(sender),
           (int)message.msg_namelen);
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo info;
            memcpy(&info, CMSG_DATA(header), sizeof info);
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &info.ipi_addr, text, sizeof text);
            printf("  IP_PKTINFO: len %d, to %s, an interface: %d\n", (int)header->cmsg_len, text, info.ipi_ifindex > 0);
        } else if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(header), sizeof info);
            char text[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &info.ipi6_addr, text, sizeof text);
            printf("  IPV6_PKTINFO: len %d, to %s, an interface: %d\n", (int)header->cmsg_len, text,
                   info.ipi6_ifindex > 0);
        } else {
            printf("  control level %d type %d\n", header->cmsg_level, header->cmsg_type);
        }
    }
    char big[100];
    memset(big, 'u', sizeof big);
    sendto(sender, big, sizeof big, 0, (struct sockaddr *)&name, length);
    report("recv of 100 into 10 bytes with MSG_TRUNC", recv(receiver, buffer, 10, MSG_TRUNC));
    // sendmmsg and recvmmsg: three datagrams of 1, 2 and 3 bytes.
    report("connect the sender", connect(sender, (struct sockaddr *)&name, length));
    struct mmsghdr out[3];
    struct iovec out_vectors[3] = {{"a", 1}, {"bb", 2}, {"ccc", 3}};
    memset(out, 0, sizeof out);
    for (int i = 0; i < 3; ++i) {
        out[i].msg_hdr.msg_iov = &out_vectors[i];
        out[i].msg_hdr.msg_iovlen = 1;
    }
    report("sendmmsg of 3", sendmmsg(sender, out, 3, 0));
    printf("  msg_len %u %u %u\n", out[0].msg_len, out[1].msg_len, out[2].msg_len);
    struct mmsghdr in[5];
    char in_buffers[5][8];
    struct iovec in_vectors[5];
    memset(in, 0, sizeof in);
    for (int i = 0; i < 5; ++i) {
        in_vectors[i].iov_base = in_buffers[i];
        in_vectors[i].iov_len = sizeof in_buffers[i];
        in[i].msg_hdr.msg_iov = &in_vectors[i];
        in[i].msg_hdr.msg_iovlen = 1;
    }
    sleep_ms(50);
    report("recvmmsg of up to 5 with MSG_DONTWAIT", recvmmsg(receiver, in, 5, MSG_DONTWAIT, NULL));
    printf("  msg_len %u %u %u\n", in[0].msg_len, in[1].msg_len, in[2].msg_len);
    close(receiver);
    // A connected datagram socket learns that nobody listens.
    report("send to the closed port", send(sender, "x", 1, 0));
    sleep_ms(50);
    report("recv", recv(sender, buffer, sizeof buffer, MSG_DONTWAIT));
    close(sender);
}

// --- epoll -------------------------------------------------------------------------------------------------

static void test_epoll(void) {
    printf("-- epoll\n");
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    int epoll = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event interest = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.fd = pair[0]};
    report("epoll_ctl", epoll_ctl(epoll, EPOLL_CTL_ADD, pair[0], &interest));
    struct epoll_event events[4];
    report("epoll_wait at the start", epoll_wait(epoll, events, 4, 0));
    printf("  EPOLLOUT %d, EPOLLIN %d\n", (events[0].events & EPOLLOUT) != 0, (events[0].events & EPOLLIN) != 0);
    write(pair[1], "e", 1);
    report("epoll_wait after a write", epoll_wait(epoll, events, 4, 0));
    printf("  EPOLLIN %d, EPOLLRDHUP %d\n", (events[0].events & EPOLLIN) != 0, (events[0].events & EPOLLRDHUP) != 0);
    shutdown(pair[1], SHUT_WR);
    report("epoll_wait after the peer's SHUT_WR", epoll_wait(epoll, events, 4, 0));
    printf("  EPOLLIN %d, EPOLLRDHUP %d, EPOLLHUP %d\n", (events[0].events & EPOLLIN) != 0,
           (events[0].events & EPOLLRDHUP) != 0, (events[0].events & EPOLLHUP) != 0);
    close(pair[1]);
    report("epoll_wait after the peer's close", epoll_wait(epoll, events, 4, 0));
    printf("  EPOLLIN %d, EPOLLRDHUP %d, EPOLLHUP %d\n", (events[0].events & EPOLLIN) != 0,
           (events[0].events & EPOLLRDHUP) != 0, (events[0].events & EPOLLHUP) != 0);
    close(epoll);
    close(pair[0]);
}

// --- Signals -----------------------------------------------------------------------------------------------

struct blocked {
    int fd;
    long result;
    int error;
};

static void *receive(void *argument) {
    struct blocked *blocked = argument;
    char buffer[8];
    blocked->result = recv(blocked->fd, buffer, sizeof buffer, 0);
    blocked->error = errno;
    return NULL;
}

static void interrupted_round(const char *what, int flags, int with_timeout) {
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    if (with_timeout) {
        struct timeval timeout = {10, 0};
        setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    }
    install(SIGUSR1, on_sigusr1, flags);
    sigusr1s = 0;
    struct blocked blocked = {pair[0], 0, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, receive, &blocked);
    sleep_ms(200);  // the thread waits in recv by now
    pthread_kill(thread, SIGUSR1);
    sleep_ms(200);
    write(pair[1], "late", 4);
    pthread_join(thread, NULL);
    printf("%s: handler ran %d, recv %ld", what, (int)sigusr1s, blocked.result);
    if (blocked.result == -1) printf(" errno %d (%s)", blocked.error, strerror(blocked.error));
    printf("\n");
    close(pair[0]);
    close(pair[1]);
}

static void test_signals(void) {
    printf("-- a blocked recv and a signal\n");
    interrupted_round("without SA_RESTART", 0, 0);
    interrupted_round("with SA_RESTART", SA_RESTART, 0);
    interrupted_round("with SA_RESTART and SO_RCVTIMEO", SA_RESTART, 1);
    install(SIGUSR1, SIG_DFL, 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (chdir("/data/local/tmp") != 0) printf("chdir failed\n");
    test_socket();
    test_socketpair_stream();
    test_socketpair_datagram();
    test_socketpair_seqpacket();
    test_rights();
    test_unix_names();
    tcp_round(AF_INET);
    tcp_round(AF_INET6);
    test_options();
    test_mapped();
    udp_round(AF_INET);
    udp_round(AF_INET6);
    test_epoll();
    test_signals();
    printf("done\n");
    return 0;
}
