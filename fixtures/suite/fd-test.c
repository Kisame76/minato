// fd-test: descriptor numbers as Linux hands them out (ADR-0027): the lowest free number, duplicates
// that share their open file, close-on-exec per number, Linux's errors, a full table, pipes, and the
// standard descriptors. It first closes every number from 3 to 1023, which must disturb neither the
// program nor whoever started it. Then signals during blocking reads and writes on pipes, and
// O_NONBLOCK through FIONBIO. Expected output: fixtures/expected/fd-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void report(const char *what, int result) {
    if (result == -1) {
        printf("%s: -1 errno %d (%s)\n", what, errno, strerror(errno));
    } else {
        printf("%s: %d\n", what, result);
    }
}

static int create(const char *name) {
    return open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
}

static void numbers(void) {
    // Whatever the starter left open above 2 goes first, as some programs do at their start.
    for (int fd = 3; fd < 1024; ++fd) close(fd);
    const int a = create("fd-a");
    const int b = create("fd-b");
    const int c = create("fd-c");
    printf("three opens: %d %d %d\n", a, b, c);
    close(b);
    const int d = create("fd-d");
    printf("open after closing %d: %d\n", b, d);
    const int copy = dup(a);
    printf("dup: %d\n", copy);
    const int from_10 = fcntl(a, F_DUPFD, 10);
    const int from_10_again = fcntl(a, F_DUPFD, 10);
    printf("F_DUPFD from 10: %d, then %d\n", from_10, from_10_again);
    const int cloexec = fcntl(a, F_DUPFD_CLOEXEC, 0);
    printf("F_DUPFD_CLOEXEC: %d, FD_CLOEXEC there %d, on the original %d\n", cloexec, fcntl(cloexec, F_GETFD),
           fcntl(a, F_GETFD));
    report("dup2 to 20", dup2(a, 20));
    report("dup3 to 21 with O_CLOEXEC", dup3(a, 21, O_CLOEXEC));
    printf("FD_CLOEXEC of 20 and 21: %d %d\n", fcntl(20, F_GETFD), fcntl(21, F_GETFD));

    // dup2 onto an open number: it refers to the other file from then on.
    report("dup2 of fd-c onto 20", dup2(c, 20));
    write(20, "via 20", 6);
    char text[16] = {0};
    lseek(c, 0, SEEK_SET);
    read(c, text, sizeof text - 1);
    printf("fd-c now holds: %s\n", text);
    report("dup2 onto itself", dup2(a, a));
    report("dup3 onto itself", dup3(a, a, 0));
    const int opened[] = {a, c, d, copy, from_10, from_10_again, cloexec, 20, 21};
    for (unsigned index = 0; index < sizeof opened / sizeof opened[0]; ++index) close(opened[index]);
}

static void sharing(void) {
    const int fd = create("fd-shared");
    const int copy = dup(fd);
    write(fd, "0123456789", 10);
    printf("offset seen through the copy: %ld\n", (long)lseek(copy, 0, SEEK_CUR));
    lseek(copy, 4, SEEK_SET);
    char text[4] = {0};
    read(fd, text, 3);
    printf("read after the copy's lseek: %s\n", text);
    fcntl(copy, F_SETFL, fcntl(copy, F_GETFL) | O_APPEND);
    printf("O_APPEND seen through the original: %d\n", (fcntl(fd, F_GETFL) & O_APPEND) != 0);
    fcntl(copy, F_SETFD, FD_CLOEXEC);
    printf("FD_CLOEXEC stays with its number: copy %d, original %d\n", fcntl(copy, F_GETFD), fcntl(fd, F_GETFD));
    close(fd);
    printf("the copy writes on after the original is closed: %d\n", (int)write(copy, "x", 1));
    close(copy);
}

static void errors(void) {
    char byte;
    report("close of a number never opened", close(900));
    report("read from it", (int)read(900, &byte, 1));
    report("dup of it", dup(900));
    report("fcntl F_GETFD of it", fcntl(900, F_GETFD));
    report("dup2 to -1", dup2(1, -1));
    report("fcntl F_DUPFD from -1", fcntl(1, F_DUPFD, -1));
    report("dup3 with O_APPEND", dup3(1, 30, O_APPEND));
}

static void pipes(void) {
    int ends[2];
    report("pipe2 with O_NONBLOCK and O_CLOEXEC", pipe2(ends, O_NONBLOCK | O_CLOEXEC));
    printf("pipe ends: %d %d\n", ends[0], ends[1]);
    char text[16] = {0};
    report("read from the empty pipe", (int)read(ends[0], text, sizeof text));
    printf("FD_CLOEXEC of both ends: %d %d\n", fcntl(ends[0], F_GETFD), fcntl(ends[1], F_GETFD));
    printf("O_NONBLOCK of the read end: %d\n", (fcntl(ends[0], F_GETFL) & O_NONBLOCK) != 0);
    write(ends[1], "ready", 5);
    report("read what was written", (int)read(ends[0], text, sizeof text));
    close(ends[0]);
    close(ends[1]);

    report("pipe", pipe(ends));
    write(ends[1], "blocking", 8);
    close(ends[1]);
    memset(text, 0, sizeof text);
    const int got = (int)read(ends[0], text, sizeof text);
    printf("a blocking read of what is there: %d %s\n", got, text);
    report("a blocking read after the writer closed", (int)read(ends[0], text, sizeof text));
    close(ends[0]);
}

static void standard(void) {
    // Closing stdout closes only the program's own: a file opened then takes number 1, and the
    // output comes back through a copy made before.
    fflush(stdout);
    const int saved = dup(1);
    close(1);
    const int file = open("fd-stdout", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    write(file, "into the file\n", 14);
    close(file);
    dup2(saved, 1);
    close(saved);
    printf("the file took number %d\n", file);
    char text[32] = {0};
    const int back = open("fd-stdout", O_RDONLY);
    read(back, text, sizeof text - 1);
    close(back);
    printf("it holds: %s", text);
}

static void full_table(void) {
    const int fd = create("fd-full");
    int last = fd;
    for (;;) {
        const int copy = dup(fd);
        if (copy < 0) break;
        last = copy;
    }
    report("dup when the table is full", dup(fd));
    report("open when the table is full", open("fd-full", O_RDONLY));
    int ends[2];
    report("pipe when the table is full", pipe(ends));
    for (int copy = fd + 1; copy <= last; ++copy) close(copy);
    const int again = dup(fd);
    report("dup after closing the copies", again);
    close(again);
    close(fd);
}

// --- Signals during blocking reads and writes ---------------------------------------------------------

static volatile sig_atomic_t handled;

static void on_signal(int signal) {
    (void)signal;
    ++handled;
}

static void install(int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    action.sa_flags = flags;
    sigaction(SIGUSR1, &action, NULL);
}

static char big[256 * 1024];  // more than a pipe holds

// A read or write that a thread of its own makes, with SIGUSR1 blocked if asked.
struct call {
    int fd;
    int writing;
    size_t size;
    int blocked;
    char byte;  // what a read of one byte got
    ssize_t result;
    int error;
};

static void *make_call(void *argument) {
    struct call *call = argument;
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (call->blocked) pthread_sigmask(SIG_BLOCK, &set, &old);
    call->result = call->writing ? write(call->fd, big, call->size) : read(call->fd, &call->byte, call->size);
    call->error = errno;
    if (call->blocked) pthread_sigmask(SIG_SETMASK, &old, NULL);  // the pending SIGUSR1 arrives here
    return NULL;
}

// Starts `call`, sends its thread SIGUSR1 50 ms later, writes `late` to `late_fd` 50 ms after that
// unless it is 0, and waits for the call.
static void interrupt(struct call *call, char late, int late_fd) {
    handled = 0;
    pthread_t thread;
    pthread_create(&thread, NULL, make_call, call);
    usleep(50 * 1000);
    pthread_kill(thread, SIGUSR1);
    if (late != 0) {
        usleep(50 * 1000);
        write(late_fd, &late, 1);
    }
    pthread_join(thread, NULL);
}

static void report_call(const char *what, const struct call *call) {
    errno = call->error;
    report(what, (int)call->result);
    printf("  handled: %d\n", (int)handled);
}

static void blocking(void) {
    int ends[2];
    pipe(ends);
    struct call call = {.fd = ends[0], .size = 1};
    install(0);
    interrupt(&call, 0, -1);
    report_call("a blocking read that a handler without SA_RESTART interrupts", &call);

    install(SA_RESTART);
    call = (struct call){.fd = ends[0], .size = 1};
    interrupt(&call, 'r', ends[1]);
    report_call("a blocking read that a handler with SA_RESTART interrupts", &call);
    printf("  it read: %c\n", call.byte);

    call = (struct call){.fd = ends[0], .size = 1, .blocked = 1};
    interrupt(&call, 'b', ends[1]);
    report_call("a blocking read while the signal is blocked", &call);
    printf("  it read: %c\n", call.byte);
    close(ends[0]);
    close(ends[1]);

    pipe(ends);
    install(0);
    call = (struct call){.fd = ends[1], .writing = 1, .size = sizeof big};
    interrupt(&call, 0, -1);
    printf("a blocking write of 256 KiB that a handler interrupts: some of it %d\n",
           call.result > 0 && call.result < (ssize_t)sizeof big);
    printf("  handled: %d\n", (int)handled);
    call = (struct call){.fd = ends[1], .writing = 1, .size = 1};
    interrupt(&call, 0, -1);
    report_call("a blocking write to the full pipe that a handler interrupts", &call);

    // FIONBIO sets and clears O_NONBLOCK, as fcntl does.
    int on = 1, off = 0;
    report("FIONBIO on the read end", ioctl(ends[0], FIONBIO, &on));
    printf("O_NONBLOCK of the read end: %d\n", (fcntl(ends[0], F_GETFL) & O_NONBLOCK) != 0);
    char byte;
    while (read(ends[0], big, sizeof big) > 0) {
    }
    report("a read of the drained pipe", (int)read(ends[0], &byte, 1));
    report("FIONBIO off", ioctl(ends[0], FIONBIO, &off));
    printf("O_NONBLOCK of the read end: %d\n", (fcntl(ends[0], F_GETFL) & O_NONBLOCK) != 0);
    close(ends[0]);
    close(ends[1]);
    signal(SIGUSR1, SIG_DFL);
}

int main(void) {
    numbers();
    sharing();
    errors();
    pipes();
    standard();
    full_table();
    blocking();
    const char *const files[] = {"fd-a", "fd-b", "fd-c", "fd-d", "fd-shared", "fd-stdout", "fd-full"};
    for (unsigned index = 0; index < sizeof files / sizeof files[0]; ++index) unlink(files[index]);
    printf("done\n");
    return 0;
}
