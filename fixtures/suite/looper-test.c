// looper-test: ALooper as Android 15's libandroid.so gives it (ADR-0028, decision 9): prepare with and
// without ALOOPER_PREPARE_ALLOW_NON_CALLBACKS, descriptors with an ident and with a callback, the
// results WAKE, CALLBACK, TIMEOUT and ERROR with the descriptor, events and data they give, a callback
// that answers 0, removeFd, wakes and signals from other threads, forThread, acquire and release, and
// the hangups of a pipe. Descriptor numbers differ between systems, so a line names the descriptor it
// got instead. Expected output: fixtures/expected/looper-test.out, the logcat lines in .err.
#include <android/looper.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Hidden by the NDK's headers since API 35, still exported for the apps built before.
extern int poll_all(int timeout, int *fd, int *events, void **data) __asm__("ALooper_pollAll");

static int data_a, data_b, garbage;
static int expected_fd = -1;  // the descriptor a line expects

static const char *data_name(void *data) {
    if (data == NULL) return "null";
    if (data == &data_a) return "a";
    if (data == &data_b) return "b";
    if (data == &garbage) return "unchanged";
    return "another";
}

static const char *fd_name(int fd) {
    if (fd == 0) return "0";
    if (fd == -77) return "unchanged";
    return fd == expected_fd ? "the one expected" : "another";
}

// Polls with out-values preset to garbage and prints what came back.
static int poll_once(const char *what, int timeout) {
    int fd = -77, events = -77;
    void *data = &garbage;
    const int result = ALooper_pollOnce(timeout, &fd, &events, &data);
    printf("%s: %d, fd %s, events %d, data %s\n", what, result, fd_name(fd), events, data_name(data));
    return result;
}

static void drain(int fd) {
    char bytes[16];
    while (read(fd, bytes, sizeof bytes) == (ssize_t)sizeof bytes) {
    }
}

// --- Callbacks ------------------------------------------------------------------------------------

static int calls, answer = 1;
static int last_fd, last_events;
static void *last_data;

static int on_event(int fd, int events, void *data) {
    ++calls;
    last_fd = fd;
    last_events = events;
    last_data = data;
    char byte;
    if (events & ALOOPER_EVENT_INPUT) (void)read(fd, &byte, 1);
    return answer;
}

static void print_callback(void) {
    printf("  callback: calls %d, fd %s, events %d, data %s\n", calls, fd_name(last_fd), last_events,
           data_name(last_data));
}

static int on_event_closing(int fd, int events, void *data) {
    (void)events;
    (void)data;
    ++calls;
    close(fd);
    return 0;
}

// --- Threads --------------------------------------------------------------------------------------

static void *without_looper(void *argument) {
    (void)argument;
    printf("forThread on a thread without a looper: %s\n", ALooper_forThread() == NULL ? "null" : "a looper");
    expected_fd = -1;
    poll_once("pollOnce on a thread without a looper", 0);
    return NULL;
}

static ALooper *main_looper;

static void *with_idents(void *argument) {
    (void)argument;
    ALooper *own = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    printf("prepare(ALLOW_NON_CALLBACKS) on another thread: %s\n",
           own == NULL ? "null" : own == main_looper ? "the main thread's" : "a looper of its own");
    int q[2], r[2], s[2], t[2];
    if (pipe(q) != 0 || pipe(r) != 0 || pipe(s) != 0 || pipe(t) != 0) return NULL;
    printf("addFd without a callback, ident 7: %d\n", ALooper_addFd(own, q[0], 7, ALOOPER_EVENT_INPUT, NULL, &data_b));
    printf("addFd without a callback, ident -5: %d\n", ALooper_addFd(own, r[0], -5, ALOOPER_EVENT_INPUT, NULL, NULL));
    expected_fd = q[0];
    (void)write(q[1], "y", 1);
    poll_once("an ident's descriptor is ready", -1);
    poll_once("its byte is still there", 0);
    drain(q[0]);
    poll_once("after the byte was read", 0);

    answer = 1;
    calls = 0;
    ALooper_addFd(own, s[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a);
    (void)write(q[1], "y", 1);
    (void)write(s[1], "z", 1);
    poll_once("an ident and a callback ready together", -1);
    printf("  callback ran before: %s\n", calls == 1 ? "yes" : "no");
    drain(q[0]);
    expected_fd = -1;
    poll_once("after both", 0);

    ALooper_addFd(own, t[0], 8, ALOOPER_EVENT_INPUT, NULL, &data_a);
    (void)write(q[1], "y", 1);
    (void)write(t[1], "y", 1);
    int fd = -77, events = -77;
    void *data = &garbage;
    const int first = ALooper_pollOnce(-1, &fd, &events, &data);
    const int second = ALooper_pollOnce(0, &fd, &events, &data);
    printf("two idents ready: %d and %d, one per poll\n", first < second ? first : second, first < second ? second : first);
    drain(q[0]);
    drain(t[0]);
    poll_once("after both were read", 0);

    int u[2];
    if (pipe(u) != 0) return NULL;
    ALooper_addFd(own, u[0], 0, ALOOPER_EVENT_INPUT, NULL, &data_a);
    expected_fd = u[0];
    (void)write(u[1], "y", 1);
    poll_once("an ident of 0", -1);
    return NULL;
}

static void *prepare_and_leave(void *argument) {
    (void)argument;
    ALooper_prepare(0);
    return NULL;
}

static void *own_and_leave(void *argument) {
    (void)argument;
    ALooper *looper = ALooper_prepare(0);
    ALooper_acquire(looper);
    return looper;  // the thread's own reference goes with the thread
}

static void *waker(void *argument) {
    usleep(50 * 1000);
    ALooper_wake((ALooper *)argument);
    return NULL;
}

static volatile sig_atomic_t signals_seen;
static volatile int poll_returned;
static pthread_t main_thread;

static void on_signal(int signal) {
    (void)signal;
    ++signals_seen;
}

// Signals the main thread until its poll has returned: one of them comes while it waits.
static void *signaler(void *argument) {
    (void)argument;
    while (!__atomic_load_n(&poll_returned, __ATOMIC_ACQUIRE)) {
        usleep(20 * 1000);
        pthread_kill(main_thread, SIGUSR1);
    }
    return NULL;
}

static void signal_during_poll(const char *what, int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    action.sa_flags = flags;
    sigaction(SIGUSR1, &action, NULL);
    signals_seen = 0;
    __atomic_store_n(&poll_returned, 0, __ATOMIC_RELEASE);
    pthread_t thread;
    pthread_create(&thread, NULL, signaler, NULL);
    expected_fd = -1;
    poll_once(what, -1);
    __atomic_store_n(&poll_returned, 1, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
    printf("  handler ran: %s\n", signals_seen > 0 ? "yes" : "no");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    main_thread = pthread_self();

    // prepare and forThread.
    printf("forThread before prepare: %s\n", ALooper_forThread() == NULL ? "null" : "a looper");
    ALooper *looper = ALooper_prepare(0);
    main_looper = looper;
    printf("prepare(0): %s\n", looper == NULL ? "null" : "a looper");
    printf("forThread after prepare: %s\n", ALooper_forThread() == looper ? "the same" : "another");
    printf("prepare(0) again: %s\n", ALooper_prepare(0) == looper ? "the same" : "another");
    printf("prepare(ALLOW_NON_CALLBACKS) again: %s\n",
           ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS) == looper ? "the same" : "another");

    // Timeouts and wakes.
    expected_fd = -1;
    poll_once("nothing ready, timeout 0", 0);
    poll_once("nothing ready, timeout 20 ms", 20);
    ALooper_wake(looper);
    poll_once("after a wake, timeout -1", -1);
    ALooper_wake(looper);
    ALooper_wake(looper);
    poll_once("after two wakes", 0);
    poll_once("and then", 0);

    // A descriptor without a callback on a looper that needs callbacks.
    int p[2];
    if (pipe(p) != 0) return 1;
    printf("addFd without a callback: %d\n", ALooper_addFd(looper, p[0], 3, ALOOPER_EVENT_INPUT, NULL, &data_a));
    printf("addFd of a descriptor that is not open: %d\n",
           ALooper_addFd(looper, 1000, 0, ALOOPER_EVENT_INPUT, on_event, NULL));

    // A callback.
    printf("addFd with a callback, ident 99: %d\n", ALooper_addFd(looper, p[0], 99, ALOOPER_EVENT_INPUT, on_event, &data_a));
    (void)write(p[1], "x", 1);
    poll_once("its descriptor is ready", -1);
    expected_fd = p[0];
    print_callback();
    expected_fd = -1;
    poll_once("after the callback read the byte", 0);
    answer = 0;
    (void)write(p[1], "x", 1);
    poll_once("a callback that answers 0", -1);
    printf("  callback calls: %d\n", calls);
    (void)write(p[1], "x", 1);
    poll_once("after it answered 0", 0);
    printf("  callback calls: %d\n", calls);
    printf("removeFd after it answered 0: %d\n", ALooper_removeFd(looper, p[0]));
    drain(p[0]);

    // removeFd.
    answer = 1;
    printf("addFd: %d\n", ALooper_addFd(looper, p[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a));
    printf("removeFd: %d\n", ALooper_removeFd(looper, p[0]));
    printf("removeFd again: %d\n", ALooper_removeFd(looper, p[0]));
    (void)write(p[1], "x", 1);
    poll_once("a removed descriptor is ready", 0);
    printf("  callback calls: %d\n", calls);
    drain(p[0]);

    // The same descriptor added twice: the second replaces the first.
    printf("addFd, data a: %d\n", ALooper_addFd(looper, p[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a));
    printf("addFd again, data b: %d\n", ALooper_addFd(looper, p[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_b));
    (void)write(p[1], "x", 1);
    poll_once("after the second addFd", -1);
    expected_fd = p[0];
    print_callback();
    printf("removeFd: %d\n", ALooper_removeFd(looper, p[0]));
    printf("removeFd again: %d\n", ALooper_removeFd(looper, p[0]));

    // Output, and the hangups of a pipe.
    answer = 0;
    ALooper_addFd(looper, p[1], 0, ALOOPER_EVENT_OUTPUT, on_event, &data_b);
    expected_fd = -1;
    poll_once("the write end is writable", 0);
    expected_fd = p[1];
    print_callback();
    int h[2];
    if (pipe(h) != 0) return 1;
    ALooper_addFd(looper, h[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a);
    close(h[1]);
    expected_fd = -1;
    poll_once("the write end was closed", -1);
    expected_fd = h[0];
    print_callback();
    close(h[0]);
    if (pipe(h) != 0) return 1;
    (void)write(h[1], "x", 1);
    close(h[1]);
    ALooper_addFd(looper, h[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a);
    expected_fd = -1;
    poll_once("closed with a byte left", -1);
    expected_fd = h[0];
    print_callback();
    close(h[0]);

    // A callback that closes its descriptor and answers 0.
    if (pipe(h) != 0) return 1;
    calls = 0;
    ALooper_addFd(looper, h[0], 0, ALOOPER_EVENT_INPUT, on_event_closing, NULL);
    (void)write(h[1], "x", 1);
    expected_fd = -1;
    poll_once("a callback that closes its descriptor", -1);
    poll_once("then", 0);
    poll_once("then", 0);
    poll_once("then", 0);
    printf("  callback calls: %d\n", calls);
    close(h[1]);

    // pollAll runs the callbacks until none is left.
    answer = 1;
    calls = 0;
    ALooper_addFd(looper, p[0], 0, ALOOPER_EVENT_INPUT, on_event, &data_a);
    (void)write(p[1], "x", 1);
    {
        int fd = -77, events = -77;
        void *data = &garbage;
        const int result = poll_all(0, &fd, &events, &data);
        printf("pollAll with a callback ready, timeout 0: %d, fd %s, events %d, data %s\n", result, fd_name(fd),
               events, data_name(data));
        printf("  callback calls: %d\n", calls);
        ALooper_wake(looper);
        printf("pollAll after a wake: %d\n", poll_all(-1, &fd, &events, &data));
    }
    ALooper_removeFd(looper, p[0]);

    // Other threads.
    pthread_t thread;
    pthread_create(&thread, NULL, without_looper, NULL);
    pthread_join(thread, NULL);
    pthread_create(&thread, NULL, with_idents, NULL);
    pthread_join(thread, NULL);
    pthread_create(&thread, NULL, waker, looper);
    expected_fd = -1;
    poll_once("a wake from another thread", -1);
    pthread_join(thread, NULL);
    void *other = NULL;
    pthread_create(&thread, NULL, own_and_leave, NULL);
    pthread_join(thread, &other);
    ALooper_wake((ALooper *)other);
    ALooper_release((ALooper *)other);
    printf("the looper of a thread that ended, acquired before: woken and released\n");
    const int free_before = dup(0);
    close(free_before);
    pthread_create(&thread, NULL, prepare_and_leave, NULL);
    pthread_join(thread, NULL);
    const int free_after = dup(0);
    close(free_after);
    printf("the looper of a thread that ended closes its descriptors: %s\n", free_after == free_before ? "yes" : "no");
    ALooper_acquire(looper);
    ALooper_release(looper);
    printf("acquire and release: %s\n", ALooper_forThread() == looper ? "still the thread's looper" : "gone");

    // Signals end the wait: epoll_wait is never restarted.
    signal_during_poll("a signal while it waits", 0);
    signal_during_poll("a signal with SA_RESTART while it waits", SA_RESTART);
    return 0;
}
