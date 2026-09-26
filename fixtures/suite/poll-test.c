// poll-test: eventfd, epoll, poll and select as Android's looper and other programs use them (ADR-0027,
// decisions 8 to 10): the counter of an eventfd, the looper's pattern of an eventfd and a pipe in an
// epoll, level-triggered, edge-triggered and one-shot interests, hangups and errors on pipes, interests
// that go with their open file, an epoll inside an epoll, and the signal masks of the waits. Expected
// output: fixtures/expected/poll-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void report(const char *what, long result) {
    if (result == -1) {
        printf("%s: -1 errno %d (%s)\n", what, errno, strerror(errno));
    } else {
        printf("%s: %ld\n", what, result);
    }
}

// --- Helpers ------------------------------------------------------------------------------------------

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

static void block_sigusr1(int how) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(how, &set, NULL);
}

static int sigusr1_blocked(void) {
    sigset_t set;
    sigprocmask(SIG_BLOCK, NULL, &set);
    return sigismember(&set, SIGUSR1);
}

// What another thread does 50 ms after it starts: send SIGUSR1 to the main thread, or write 8 bytes
// (an eventfd's increment) or one byte to a descriptor.
struct later {
    pthread_t thread;
    pthread_t target;
    int fd;         // -1: send SIGUSR1 to `target`
    uint64_t value; // written to `fd`, 8 bytes, or 1 byte with `byte`
    int byte;
};

static void *act(void *argument) {
    struct later *later = argument;
    usleep(50 * 1000);
    if (later->fd < 0) {
        pthread_kill(later->target, SIGUSR1);
    } else if (later->byte) {
        write(later->fd, "b", 1);
    } else {
        write(later->fd, &later->value, sizeof later->value);
    }
    return NULL;
}

static void start(struct later *later) {
    later->target = pthread_self();
    pthread_create(&later->thread, NULL, act, later);
}

static void finish(struct later *later) {
    pthread_join(later->thread, NULL);
}

static void close_all(const int *fds, int count) {
    for (int index = 0; index < count; ++index) close(fds[index]);
}

// Waits up to `timeout` ms for events on `epoll` and prints them in the order of their data, which
// needs no order among events that are ready together.
static void show_events(const char *what, int epoll, int timeout) {
    struct epoll_event events[8];
    const int count = epoll_wait(epoll, events, 8, timeout);
    if (count < 0) {
        report(what, -1);
        return;
    }
    for (int index = 1; index < count; ++index) {
        for (int other = index; other > 0 && events[other - 1].data.u64 > events[other].data.u64; --other) {
            const struct epoll_event swap = events[other];
            events[other] = events[other - 1];
            events[other - 1] = swap;
        }
    }
    printf("%s: %d", what, count);
    for (int index = 0; index < count; ++index) {
        printf(" [%#x for %llu]", events[index].events, (unsigned long long)events[index].data.u64);
    }
    printf("\n");
}

static int watch(int epoll, int fd, uint32_t events, uint64_t data) {
    struct epoll_event event = {.events = events, .data.u64 = data};
    return epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event);
}

static int change(int epoll, int fd, uint32_t events, uint64_t data) {
    struct epoll_event event = {.events = events, .data.u64 = data};
    return epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &event);
}

// --- eventfd ------------------------------------------------------------------------------------------

static void counter(void) {
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t value = 0;
    report("read of an eventfd at 0", read(fd, &value, sizeof value));
    value = 1;
    write(fd, &value, sizeof value);
    value = 2;
    write(fd, &value, sizeof value);
    report("read after writes of 1 and 2", read(fd, &value, sizeof value));
    printf("  value %llu\n", (unsigned long long)value);
    report("read again", read(fd, &value, sizeof value));

    char small[4] = {0};
    report("read of 4 bytes", read(fd, small, sizeof small));
    report("write of 4 bytes", write(fd, small, sizeof small));
    const uint64_t pair[2] = {5, 7};
    report("write of 16 bytes", write(fd, pair, sizeof pair));
    uint64_t large[2] = {0, 0};
    report("read of 16 bytes", read(fd, large, sizeof large));
    printf("  value %llu\n", (unsigned long long)large[0]);

    value = UINT64_MAX;
    report("write of 2^64 - 1", write(fd, &value, sizeof value));
    value = UINT64_MAX - 1;
    report("write of 2^64 - 2", write(fd, &value, sizeof value));
    value = 1;
    report("write of 1 more", write(fd, &value, sizeof value));
    report("read at the maximum", read(fd, &value, sizeof value));
    printf("  value is 2^64 - 2: %d\n", value == UINT64_MAX - 1);

    const int flags = fcntl(fd, F_GETFL);
    printf("F_GETFL: access mode %d, O_NONBLOCK %d; FD_CLOEXEC %d\n", flags & O_ACCMODE, (flags & O_NONBLOCK) != 0,
           fcntl(fd, F_GETFD));
    struct stat status;
    fstat(fd, &status);
    printf("fstat: mode %o, size %lld\n", status.st_mode, (long long)status.st_size);
    report("lseek", lseek(fd, 0, SEEK_CUR));
    close(fd);

    const int semaphore = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    for (int round = 0; round < 3; ++round) {
        value = 0;
        report("read of a semaphore that started at 2", read(semaphore, &value, sizeof value));
        printf("  value %llu\n", (unsigned long long)value);
    }
    close(semaphore);
    report("eventfd with an unknown flag", syscall(__NR_eventfd2, 0, 8));

    // A blocking read waits for the write that another thread makes.
    const int blocking = eventfd(0, 0);
    struct later writer = {.fd = blocking, .value = 42};
    start(&writer);
    value = 0;
    report("a blocking read", read(blocking, &value, sizeof value));
    printf("  value %llu\n", (unsigned long long)value);
    finish(&writer);
    close(blocking);
}

// --- The looper's pattern -----------------------------------------------------------------------------

enum { wake_data = 1, request_data = 2 };

static void looper(void) {
    // As Looper.cpp: a non-blocking eventfd to wake it, an epoll with that eventfd and a request's pipe.
    const int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const int epoll = epoll_create1(EPOLL_CLOEXEC);
    int request[2];
    pipe2(request, O_CLOEXEC);
    report("add the wake eventfd", watch(epoll, wake, EPOLLIN, wake_data));
    report("add the request's pipe", watch(epoll, request[0], EPOLLIN, request_data));
    show_events("poll once without waiting", epoll, 0);

    struct later waker = {.fd = wake, .value = 1};
    start(&waker);
    show_events("a wake from another thread", epoll, 5000);
    finish(&waker);
    uint64_t value = 0;
    report("awoken", read(wake, &value, sizeof value));
    show_events("nothing after awoken", epoll, 0);

    write(request[1], "r", 1);
    value = 1;
    write(wake, &value, sizeof value);
    show_events("a request and a wake", epoll, 5000);
    show_events("both still there, level-triggered", epoll, 0);
    read(wake, &value, sizeof value);
    char byte;
    read(request[0], &byte, 1);
    show_events("a wait of 20 ms", epoll, 20);

    struct later requester = {.fd = request[1], .byte = 1};
    start(&requester);
    show_events("a request from another thread", epoll, 5000);
    finish(&requester);
    read(request[0], &byte, 1);
    close(request[1]);
    show_events("the request's writer is gone", epoll, 0);
    close(request[0]);
    close(wake);
    close(epoll);
}

// --- Interests ------------------------------------------------------------------------------------------

static void modes(void) {
    const int epoll = epoll_create1(0);
    int ends[2];
    pipe(ends);
    watch(epoll, ends[0], EPOLLIN | EPOLLET, 10);
    write(ends[1], "a", 1);
    show_events("edge-triggered", epoll, 0);
    show_events("edge-triggered again", epoll, 0);
    write(ends[1], "b", 1);
    show_events("edge-triggered after another byte", epoll, 0);

    change(epoll, ends[0], EPOLLIN | EPOLLONESHOT, 11);
    show_events("one-shot", epoll, 0);
    show_events("one-shot again", epoll, 0);
    write(ends[1], "c", 1);
    show_events("one-shot after another byte", epoll, 0);
    change(epoll, ends[0], EPOLLIN | EPOLLONESHOT, 12);
    show_events("one-shot re-armed", epoll, 0);

    change(epoll, ends[0], EPOLLIN, 13);
    show_events("level-triggered", epoll, 0);
    show_events("level-triggered again", epoll, 0);
    close_all(ends, 2);

    // An eventfd is always writable, and readable while its counter is not 0.
    const int counter = eventfd(0, EFD_NONBLOCK);
    watch(epoll, counter, EPOLLIN | EPOLLOUT, 14);
    show_events("an eventfd at 0", epoll, 0);
    uint64_t value = 1;
    write(counter, &value, sizeof value);
    show_events("an eventfd at 1", epoll, 0);
    close(counter);
    close(epoll);
}

static void pipes(void) {
    const int epoll = epoll_create1(0);
    int ends[2];
    pipe(ends);
    watch(epoll, ends[0], EPOLLIN, 20);
    watch(epoll, ends[1], EPOLLOUT, 21);
    show_events("an empty pipe", epoll, 0);
    write(ends[1], "ab", 2);
    close(ends[1]);
    show_events("two bytes, the writer gone", epoll, 0);
    char text[2];
    read(ends[0], text, sizeof text);
    show_events("empty, the writer gone", epoll, 0);
    close(ends[0]);

    pipe(ends);
    watch(epoll, ends[1], EPOLLOUT, 22);
    close(ends[0]);
    show_events("the reader gone", epoll, 0);
    close(ends[1]);
    close(epoll);
}

static void errors(void) {
    const int epoll = epoll_create1(0);
    const int file = open("poll-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    const int null = open("/dev/null", O_RDWR);
    const int directory = open(".", O_RDONLY | O_DIRECTORY);
    const int counter = eventfd(0, 0), other = eventfd(0, 0);
    report("add a regular file", watch(epoll, file, EPOLLIN, 0));
    report("add /dev/null", watch(epoll, null, EPOLLIN, 0));
    report("add a directory", watch(epoll, directory, EPOLLIN, 0));
    report("add the epoll itself", watch(epoll, epoll, EPOLLIN, 0));
    report("add a number that is not open", watch(epoll, 900, EPOLLIN, 0));
    report("add an eventfd", watch(epoll, counter, EPOLLIN, 0));
    report("add it again", watch(epoll, counter, EPOLLIN, 0));
    report("change a number not in it", change(epoll, other, EPOLLIN, 0));
    report("remove a number not in it", epoll_ctl(epoll, EPOLL_CTL_DEL, other, NULL));
    struct epoll_event any = {.events = EPOLLIN};
    report("an unknown operation", epoll_ctl(epoll, 99, counter, &any));
    report("remove the eventfd", epoll_ctl(epoll, EPOLL_CTL_DEL, counter, NULL));
    struct epoll_event events[1];
    report("a wait for 0 events", epoll_wait(epoll, events, 0, 0));
    report("a wait on an eventfd", epoll_wait(counter, events, 1, 0));
    report("epoll_create1 with an unknown flag", epoll_create1(1));
    const int opened[] = {epoll, file, null, directory, counter, other};
    close_all(opened, 6);
    unlink("poll-file");
}

static void closing(void) {
    // An interest belongs to a number and its open file: it goes with the file, not with the number.
    const int epoll = epoll_create1(0);
    int ends[2];
    pipe(ends);
    const int copy = dup(ends[0]);
    watch(epoll, ends[0], EPOLLIN, 30);
    report("add the copy", watch(epoll, copy, EPOLLIN, 31));
    write(ends[1], "c", 1);
    show_events("both numbers", epoll, 0);
    close(ends[0]);
    show_events("the first number closed", epoll, 0);
    report("remove the closed number", epoll_ctl(epoll, EPOLL_CTL_DEL, ends[0], NULL));
    close(copy);
    show_events("every number closed", epoll, 0);
    int again[2];
    pipe(again);
    write(again[1], "a", 1);
    printf("a new pipe takes the first number: %d\n", again[0] == ends[0]);
    report("add the new pipe there", watch(epoll, again[0], EPOLLIN, 32));
    show_events("the new pipe", epoll, 0);
    close_all(again, 2);
    close(ends[1]);
    close(epoll);
}

static void nesting(void) {
    const int inner = epoll_create1(0), outer = epoll_create1(0);
    int ends[2];
    pipe(ends);
    watch(inner, ends[0], EPOLLIN, 40);
    report("add the inner epoll to the outer", watch(outer, inner, EPOLLIN, 41));
    show_events("the outer, the inner has nothing", outer, 0);
    write(ends[1], "n", 1);
    show_events("the outer, the inner has a byte", outer, 0);
    show_events("the inner", inner, 0);
    report("add the outer epoll to the inner", watch(inner, outer, EPOLLIN, 42));
    const int opened[] = {inner, outer, ends[0], ends[1]};
    close_all(opened, 4);
}

// --- Signals ----------------------------------------------------------------------------------------

static void epoll_signals(void) {
    const int epoll = epoll_create1(0);
    int ends[2];
    pipe(ends);
    watch(epoll, ends[0], EPOLLIN, 50);

    // A handler with SA_RESTART does not restart an epoll wait.
    install(SA_RESTART);
    handled = 0;
    struct later sender = {.fd = -1};
    start(&sender);
    show_events("a wait that a handler with SA_RESTART interrupts", epoll, 5000);
    finish(&sender);
    printf("  handled: %d\n", (int)handled);

    // A signal the thread blocks ends no wait.
    handled = 0;
    block_sigusr1(SIG_BLOCK);
    start(&sender);
    show_events("a wait of 150 ms while the signal is blocked", epoll, 150);
    finish(&sender);
    printf("  handled: %d\n", (int)handled);
    block_sigusr1(SIG_UNBLOCK);
    printf("  handled once it is unblocked: %d\n", (int)handled);

    // epoll_pwait's mask: during the wait only.
    handled = 0;
    sigset_t blocking;
    sigemptyset(&blocking);
    sigaddset(&blocking, SIGUSR1);
    start(&sender);
    struct epoll_event events[1];
    report("epoll_pwait of 150 ms with a mask that blocks the signal", epoll_pwait(epoll, events, 1, 150, &blocking));
    finish(&sender);
    printf("  handled after it: %d\n", (int)handled);

    handled = 0;
    block_sigusr1(SIG_BLOCK);
    raise(SIGUSR1);
    sigset_t open;
    sigemptyset(&open);
    report("epoll_pwait with a mask that lets the pending signal in", epoll_pwait(epoll, events, 1, 5000, &open));
    printf("  handled: %d, blocked again after: %d\n", (int)handled, sigusr1_blocked());
    block_sigusr1(SIG_UNBLOCK);

    // With no time to wait, epoll_pwait looks at the time first: the signal stays pending.
    handled = 0;
    block_sigusr1(SIG_BLOCK);
    raise(SIGUSR1);
    report("epoll_pwait of no time with a mask that lets the pending signal in", epoll_pwait(epoll, events, 1, 0, &open));
    printf("  handled: %d\n", (int)handled);
    block_sigusr1(SIG_UNBLOCK);
    printf("  handled once it is unblocked: %d\n", (int)handled);
    close_all(ends, 2);
    close(epoll);
}

// --- poll and select ----------------------------------------------------------------------------------

static void show_poll(const char *what, struct pollfd *fds, int count, int timeout) {
    for (int index = 0; index < count; ++index) fds[index].revents = 0;
    const int ready = poll(fds, (nfds_t)count, timeout);
    if (ready < 0) {
        report(what, -1);
        return;
    }
    printf("%s: %d", what, ready);
    for (int index = 0; index < count; ++index) printf(" %#x", (unsigned)(unsigned short)fds[index].revents);
    printf("\n");
}

static void polls(void) {
    int ends[2];
    pipe(ends);
    const int counter = eventfd(0, EFD_NONBLOCK);
    struct pollfd fds[3] = {{ends[0], POLLIN, 0}, {counter, POLLIN, 0}, {ends[1], POLLOUT, 0}};
    show_poll("poll of a pipe, an eventfd and the pipe's write end", fds, 3, 0);
    fds[2].events = 0;
    show_poll("nothing ready", fds, 3, 0);
    struct later writer = {.fd = counter, .value = 3};
    start(&writer);
    show_poll("an eventfd written by another thread", fds, 3, 5000);
    finish(&writer);
    write(ends[1], "p", 1);
    show_poll("the pipe as well", fds, 3, 0);
    close(ends[1]);
    fds[2].fd = -1;
    show_poll("the writer gone", fds, 3, 0);
    char byte;
    read(ends[0], &byte, 1);
    fds[1].fd = ends[1];  // closed: POLLNVAL
    show_poll("empty, and a number that is not open", fds, 3, 0);

    const int file = open("poll-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    const int null = open("/dev/null", O_RDWR);
    struct pollfd always[2] = {{file, POLLIN | POLLOUT, 0}, {null, POLLIN | POLLOUT, 0}};
    show_poll("a regular file and /dev/null", always, 2, 5000);

    // The remaining time, which the syscall writes back (bionic's poll hides it).
    struct timespec timeout = {0, 50 * 1000 * 1000};
    report("ppoll of 50 ms", syscall(__NR_ppoll, NULL, 0, &timeout, NULL, 8));
    printf("  time left: %ld.%09ld\n", (long)timeout.tv_sec, timeout.tv_nsec);
    timeout = (struct timespec){5, 0};
    report("ppoll of 5 s on a regular file", syscall(__NR_ppoll, always, 1, &timeout, NULL, 8));
    printf("  time left between 4 and 5 s: %d\n", timeout.tv_sec == 4);
    install(SA_RESTART);
    handled = 0;
    struct later sender = {.fd = -1};
    start(&sender);
    timeout = (struct timespec){5, 0};
    report("ppoll of 5 s that a handler with SA_RESTART interrupts", syscall(__NR_ppoll, NULL, 0, &timeout, NULL, 8));
    finish(&sender);
    printf("  handled: %d, time left between 4 and 5 s: %d\n", (int)handled, timeout.tv_sec == 4);
    timeout = (struct timespec){0, 1000 * 1000 * 1000};
    report("ppoll with 10^9 ns", syscall(__NR_ppoll, NULL, 0, &timeout, NULL, 8));

    // ppoll's mask lets a pending signal in, for the wait only.
    handled = 0;
    block_sigusr1(SIG_BLOCK);
    raise(SIGUSR1);
    sigset_t open;
    sigemptyset(&open);
    timeout = (struct timespec){5, 0};
    report("ppoll with a mask that lets the pending signal in", ppoll(NULL, 0, &timeout, &open));
    printf("  handled: %d, blocked again after: %d\n", (int)handled, sigusr1_blocked());
    // ppoll and pselect look for signals before the time, even with no time to wait.
    handled = 0;
    raise(SIGUSR1);
    timeout = (struct timespec){0, 0};
    report("ppoll of no time with a mask that lets the pending signal in", ppoll(NULL, 0, &timeout, &open));
    printf("  handled: %d\n", (int)handled);
    handled = 0;
    raise(SIGUSR1);
    timeout = (struct timespec){0, 0};
    report("pselect of no time with a mask that lets the pending signal in",
           pselect(0, NULL, NULL, NULL, &timeout, &open));
    printf("  handled: %d\n", (int)handled);
    block_sigusr1(SIG_UNBLOCK);

    // select: the sets say what is ready, the except set nothing here; bionic's select writes the time
    // left back.
    pipe(ends);
    write(ends[1], "s", 1);
    fd_set read_set, write_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);
    FD_SET(ends[0], &read_set);
    FD_SET(file, &read_set);
    FD_SET(ends[0], &write_set);
    FD_SET(ends[1], &write_set);
    FD_SET(file, &write_set);
    FD_SET(ends[0], &except_set);
    FD_SET(file, &except_set);
    struct timeval time = {5, 0};
    const int highest = ends[0] > ends[1] ? (ends[0] > file ? ends[0] : file) : (ends[1] > file ? ends[1] : file);
    report("select", select(highest + 1, &read_set, &write_set, &except_set, &time));
    printf("  read: pipe %d, file %d; write: read end %d, write end %d, file %d; except: pipe %d, file %d\n",
           FD_ISSET(ends[0], &read_set), FD_ISSET(file, &read_set), FD_ISSET(ends[0], &write_set),
           FD_ISSET(ends[1], &write_set), FD_ISSET(file, &write_set), FD_ISSET(ends[0], &except_set),
           FD_ISSET(file, &except_set));
    printf("  time left between 4 and 5 s: %d\n", time.tv_sec == 4);
    // A number below 64: Linux leaves the bits of numbers beyond its table's size alone.
    const int closed = dup(0);
    close(closed);
    FD_ZERO(&read_set);
    FD_SET(closed, &read_set);
    time = (struct timeval){0, 0};
    report("select with a number that is not open", select(closed + 1, &read_set, NULL, NULL, &time));
    time = (struct timeval){0, 50 * 1000};
    report("select of 50 ms", select(0, NULL, NULL, NULL, &time));
    printf("  time left: %ld.%06ld\n", (long)time.tv_sec, (long)time.tv_usec);
    close_all(ends, 2);

    // pselect's mask, as ppoll's.
    handled = 0;
    block_sigusr1(SIG_BLOCK);
    raise(SIGUSR1);
    timeout = (struct timespec){5, 0};
    report("pselect with a mask that lets the pending signal in", pselect(0, NULL, NULL, NULL, &timeout, &open));
    printf("  handled: %d, blocked again after: %d\n", (int)handled, sigusr1_blocked());
    block_sigusr1(SIG_UNBLOCK);

    close(counter);
    close(file);
    close(null);
    unlink("poll-file");
    signal(SIGUSR1, SIG_DFL);
}

int main(void) {
    counter();
    looper();
    modes();
    pipes();
    errors();
    closing();
    nesting();
    epoll_signals();
    polls();
    printf("done\n");
    return 0;
}
