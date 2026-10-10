// sendfile-test: sendfile(2) as Android 15's kernel answers it, which FileUtils.copy and FileChannel.transferTo try
// first: from a file to a file, a socket and a pipe, with an offset (which moves, and not the file's) and without
// (which moves the file's), at and beyond the end, a count of 0, and its errors in the kernel's order: the offset's
// EFAULT first, then the input's EBADF, ESPIPE and EINVAL, then the output's EBADF and O_APPEND's EINVAL. A
// non-blocking socket that fills: some bytes, then EAGAIN.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIZE 100000

static char data[SIZE];

// errno's name, for the ones sendfile gives.
static const char *name_of(int error) {
    switch (error) {
        case EBADF: return "EBADF";
        case EINVAL: return "EINVAL";
        case ESPIPE: return "ESPIPE";
        case EFAULT: return "EFAULT";
        case EAGAIN: return "EAGAIN";
        case EPIPE: return "EPIPE";
        case EOVERFLOW: return "EOVERFLOW";
        case ENOSYS: return "ENOSYS";
        default: {
            static char number[16];
            snprintf(number, sizeof number, "errno %d", error);
            return number;
        }
    }
}

static void show(const char *what, ssize_t result) {
    if (result < 0) {
        printf("%s: -1 %s\n", what, name_of(errno));
    } else {
        printf("%s: %zd\n", what, result);
    }
}

static unsigned fnv(const char *bytes, size_t length) {
    unsigned hash = 2166136261u;
    for (size_t each = 0; each < length; ++each) hash = (hash ^ (unsigned char)bytes[each]) * 16777619u;
    return hash;
}

// The file `path`'s size and whether its bytes are data's from `from`.
static void check_copy(const char *what, const char *path, size_t from) {
    static char back[SIZE];
    int fd = open(path, O_RDONLY);
    ssize_t got = read(fd, back, sizeof back);
    close(fd);
    printf("%s: %zd bytes, the input's from %zu: %s\n", what, got, from,
           got >= 0 && (size_t)got <= SIZE - from && memcmp(back, data + from, (size_t)got) == 0 ? "yes" : "no");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (size_t each = 0; each < SIZE; ++each) data[each] = (char)(each * 7 + each / 251);
    printf("input digest: %08x\n", fnv(data, SIZE));
    const char *in_path = "/data/local/tmp/sendfile-in";
    const char *out_path = "/data/local/tmp/sendfile-out";
    int in = open(in_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    write(in, data, SIZE);
    lseek(in, 0, SEEK_SET);

    // A file to a file, with an offset: the offset moves, the file's position does not.
    int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    off_t offset = 100;
    show("file to file from 100, 10000 bytes", sendfile(out, in, &offset, 10000));
    printf("offset after: %lld, the input's position: %lld, the output's: %lld\n", (long long)offset,
           (long long)lseek(in, 0, SEEK_CUR), (long long)lseek(out, 0, SEEK_CUR));
    close(out);
    check_copy("output", out_path, 100);

    // Without an offset: the file's position moves; then beyond the end, and at it.
    out = open(out_path, O_WRONLY | O_TRUNC);
    lseek(in, 50000, SEEK_SET);
    show("file to file from the position 50000, 20000 bytes", sendfile(out, in, NULL, 20000));
    printf("the input's position: %lld\n", (long long)lseek(in, 0, SEEK_CUR));
    show("40000 more, beyond the end", sendfile(out, in, NULL, 40000));
    show("at the end", sendfile(out, in, NULL, 1000));
    close(out);
    check_copy("output", out_path, 50000);
    offset = SIZE + 10;
    out = open(out_path, O_WRONLY | O_TRUNC);
    show("an offset beyond the end", sendfile(out, in, &offset, 100));
    show("a count of 0", sendfile(out, in, NULL, 0));

    // To a socket and to a pipe.
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    offset = 0;
    const ssize_t to_socket = sendfile(pair[0], in, &offset, 50000);
    show("file to a socket, 50000 bytes", to_socket);
    static char received[SIZE];
    size_t total = 0;
    while (to_socket > 0 && total < (size_t)to_socket) {
        ssize_t got = read(pair[1], received + total, (size_t)to_socket - total);
        if (got <= 0) break;
        total += (size_t)got;
    }
    printf("the socket's bytes: %zu, the input's: %s\n", total, memcmp(received, data, total) == 0 ? "yes" : "no");
    int pipes[2];
    pipe(pipes);
    offset = 1000;
    const ssize_t to_pipe = sendfile(pipes[1], in, &offset, 4096);
    show("file to a pipe, 4096 bytes", to_pipe);
    ssize_t got = to_pipe > 0 ? read(pipes[0], received, (size_t)to_pipe) : 0;
    printf("the pipe's bytes: %zd, the input's: %s\n", got,
           got > 0 && memcmp(received, data + 1000, (size_t)got) == 0 ? "yes" : "no");

    // The errors, in the kernel's order.
    show("an offset at an address not mapped, from no descriptor", sendfile(out, -1, (off_t *)8, 10));
    show("from no descriptor", sendfile(out, -1, NULL, 10));
    int write_only = open(in_path, O_WRONLY);
    show("from a file open for writing only", sendfile(out, write_only, NULL, 10));
    show("from a pipe with an offset", sendfile(out, pipes[0], &offset, 10));
    write(pipes[1], "abc", 3);
    show("from a pipe", sendfile(out, pipes[0], NULL, 10));
    write(pair[1], "abc", 3);
    show("from a socket", sendfile(out, pair[0], NULL, 10));
    offset = -1;
    show("a negative offset", sendfile(out, in, &offset, 10));
    show("to no descriptor", sendfile(-1, in, NULL, 10));
    int read_only = open(out_path, O_RDONLY);
    show("to a file open for reading only", sendfile(read_only, in, NULL, 10));
    int appending = open(out_path, O_WRONLY | O_APPEND);
    offset = 0;
    show("to a file open for appending", sendfile(appending, in, &offset, 10));
    printf("offset after the error: %lld\n", (long long)offset);

    // A non-blocking socket that fills: some bytes, then EAGAIN.
    int full[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, full);
    fcntl(full[0], F_SETFL, O_NONBLOCK);
    ssize_t sent = 0, last = 0;
    int rounds = 0;
    offset = 0;
    while (rounds++ < 1000 && (last = sendfile(full[0], in, &offset, SIZE)) > 0) {
        sent += last;
        if (offset >= SIZE) offset = 0;
    }
    printf("a non-blocking socket: bytes before it filled: %s, then: %s\n", sent > 0 ? "yes" : "no",
           last < 0 ? name_of(errno) : "no error");
    return 0;
}
