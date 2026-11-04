// timerfd-test: timerfd_create, timerfd_settime and timerfd_gettime as event loops use them (Flutter's message loop,
// libuv, SDL): a one-shot timer, a periodic one, the count a read returns, polling and epoll, non-blocking reads,
// disarming and re-arming, absolute times, and the errors. Times print as buckets, so that two runs and two systems
// agree. Expected output: fixtures/expected/timerfd-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static void report(const char *what, long result) {
    if (result == -1) {
        printf("%s: -1 errno %d (%s)\n", what, errno, strerror(errno));
    } else {
        printf("%s: %ld\n", what, result);
    }
}

static void sleep_ms(int ms) {
    struct timespec time = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&time, &time) == -1 && errno == EINTR) {
    }
}

static struct itimerspec spec(long value_ms, long interval_ms) {
    struct itimerspec timer = {{interval_ms / 1000, (interval_ms % 1000) * 1000000L},
                               {value_ms / 1000, (value_ms % 1000) * 1000000L}};
    return timer;
}

static const char *bucket(long ms, long expected) {
    if (ms < expected / 2) return "far too early";
    if (ms < expected - expected / 10) return "early";
    if (ms <= expected + expected / 2) return "on time";
    return "late";
}

int main(void) {
    // A one-shot timer on the monotonic clock, non-blocking.
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    printf("create: %s\n", fd >= 0 ? "ok" : "failed");
    uint64_t count = 0;
    report("read of a timer that was never armed", read(fd, &count, 8));

    struct itimerspec timer = spec(100, 0);
    report("settime 100 ms", timerfd_settime(fd, 0, &timer, NULL));
    struct pollfd wait = {fd, POLLIN, 0};
    printf("poll at once: %d\n", poll(&wait, 1, 0));
    struct itimerspec left;
    report("gettime", timerfd_gettime(fd, &left));
    long left_ms = left.it_value.tv_sec * 1000 + left.it_value.tv_nsec / 1000000;
    printf("remaining about 100 ms: %s, interval %ld\n", left_ms > 0 && left_ms <= 100 ? "yes" : "no",
           (long)left.it_interval.tv_nsec);
    report("read before it expires", read(fd, &count, 8));
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    wait.revents = 0;
    printf("poll for 2 s: %d\n", poll(&wait, 1, 2000));
    clock_gettime(CLOCK_MONOTONIC, &after);
    long waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    printf("it expired %s, revents %#x\n", bucket(waited, 100), wait.revents);
    report("read after it expired", read(fd, &count, 8));
    printf("  count %llu\n", (unsigned long long)count);
    report("read again", read(fd, &count, 8));
    report("gettime after the expiry", timerfd_gettime(fd, &left));
    printf("  disarmed: %s\n", left.it_value.tv_sec == 0 && left.it_value.tv_nsec == 0 ? "yes" : "no");

    // A periodic timer: the read returns the expirations since the last read.
    timer = spec(20, 20);
    report("settime 20 ms, every 20 ms", timerfd_settime(fd, 0, &timer, NULL));
    sleep_ms(210);
    report("read after 210 ms", read(fd, &count, 8));
    printf("  count %s\n", count >= 6 && count <= 11 ? "between 6 and 11" : "out of range");
    report("gettime of a periodic timer", timerfd_gettime(fd, &left));
    printf("  interval %ld ms, remaining within it: %s\n", left.it_interval.tv_nsec / 1000000,
           left.it_value.tv_sec == 0 && left.it_value.tv_nsec > 0 && left.it_value.tv_nsec <= 20000000L ? "yes"
                                                                                                         : "no");
    // settime resets the count and returns the old setting.
    sleep_ms(60);
    struct itimerspec old;
    timer = spec(5000, 0);
    report("settime 5 s", timerfd_settime(fd, 0, &timer, &old));
    printf("  old interval %ld ms\n", old.it_interval.tv_nsec / 1000000);
    report("read after settime", read(fd, &count, 8));
    timer = spec(0, 0);
    report("disarm", timerfd_settime(fd, 0, &timer, &old));
    printf("  old remaining about 5 s: %s\n", old.it_value.tv_sec >= 4 ? "yes" : "no");
    report("gettime when disarmed", timerfd_gettime(fd, &left));
    printf("  zero: %s\n", left.it_value.tv_sec == 0 && left.it_value.tv_nsec == 0 ? "yes" : "no");

    // Absolute time on the realtime clock, and a time in the past.
    int real = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    timer = spec(0, 0);
    timer.it_value = now;
    timer.it_value.tv_nsec += 50000000L;
    if (timer.it_value.tv_nsec >= 1000000000L) {
        timer.it_value.tv_nsec -= 1000000000L;
        timer.it_value.tv_sec += 1;
    }
    report("settime absolute, 50 ms ahead", timerfd_settime(real, TFD_TIMER_ABSTIME, &timer, NULL));
    wait.fd = real;
    wait.revents = 0;
    printf("poll for 2 s: %d\n", poll(&wait, 1, 2000));
    report("read", read(real, &count, 8));
    printf("  count %llu\n", (unsigned long long)count);
    timer.it_value = now;
    timer.it_value.tv_sec -= 10;
    report("settime absolute, in the past", timerfd_settime(real, TFD_TIMER_ABSTIME, &timer, NULL));
    sleep_ms(50);  // the expiry has happened by then; whether it had at once is a race, so no poll before the sleep
    report("read", read(real, &count, 8));
    printf("  count %llu\n", (unsigned long long)count);
    close(real);

    // epoll: level-triggered, until the count is read.
    timer = spec(30, 0);
    timerfd_settime(fd, 0, &timer, NULL);
    int epoll = epoll_create1(0);
    struct epoll_event event = {EPOLLIN, {.fd = fd}}, got;
    report("epoll_ctl add", epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event));
    printf("epoll_wait at once: %d\n", epoll_wait(epoll, &got, 1, 0));
    printf("epoll_wait for 2 s: %d\n", epoll_wait(epoll, &got, 1, 2000));
    printf("  events %#x, fd matches: %s\n", got.events, got.data.fd == fd ? "yes" : "no");
    printf("epoll_wait again, level-triggered: %d\n", epoll_wait(epoll, &got, 1, 0));
    report("read", read(fd, &count, 8));
    printf("epoll_wait after the read: %d\n", epoll_wait(epoll, &got, 1, 0));
    close(epoll);

    // The errors.
    char small[4];
    report("read of 4 bytes", read(fd, small, 4));
    uint64_t one = 1;
    report("write", write(fd, &one, 8));
    report("create with a bad clock", timerfd_create(12345, 0));
    report("create with a bad flag", timerfd_create(CLOCK_MONOTONIC, 0x40000000));
    report("create on CLOCK_BOOTTIME", (long)(timerfd_create(CLOCK_BOOTTIME, 0) >= 0));
    timer = spec(0, 0);
    timer.it_value.tv_nsec = 1000000000L;
    report("settime with a bad nanosecond", timerfd_settime(fd, 0, &timer, NULL));
    timer = spec(10, 0);
    report("settime with a bad flag", timerfd_settime(fd, 0x40, &timer, NULL));
    report("settime on a bad descriptor", timerfd_settime(1000, 0, &timer, NULL));
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        report("settime on a pipe", timerfd_settime(pipefd[0], 0, &timer, NULL));
        report("gettime on a pipe", timerfd_gettime(pipefd[0], &left));
    }

    // A blocking timerfd: read waits for the expiry.
    int blocking = timerfd_create(CLOCK_MONOTONIC, 0);
    timer = spec(100, 0);
    timerfd_settime(blocking, 0, &timer, NULL);
    clock_gettime(CLOCK_MONOTONIC, &before);
    report("blocking read", read(blocking, &count, 8));
    clock_gettime(CLOCK_MONOTONIC, &after);
    waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    printf("  it waited %s, count %llu\n", bucket(waited, 100), (unsigned long long)count);
    close(blocking);
    close(fd);
    printf("done\n");
    return 0;
}
