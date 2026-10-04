// stw.h: a stop-the-world as Boehm's collector makes it on Android (bdwgc's pthread_stop_world.c, which IL2CPP's
// games run), shared by the fixtures stw-test and libstress-test (ADR-0033 test 4, part c):
//   - a collecting thread sends SIGPWR to every other registered thread and waits on a semaphore until each handler
//     has posted it;
//   - each handler then waits in sigsuspend, with everything but SIGXCPU blocked, until the world runs again;
//   - while the world stands, the collector checks that no stopped thread's work count moves;
//   - it restarts the world with SIGXCPU and waits until each handler has posted the semaphore again.
// Both handlers block every signal but the faults and restart interrupted calls (sa_mask, SA_RESTART), as bdwgc's
// do. By turns the stop handler is late: it reaches sigsuspend only after the restart was sent, which must wait,
// blocked, until then. After sigsuspend it checks that SIGXCPU is blocked again.
#ifndef STW_H
#define STW_H

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    stw_max_threads = 16,
    stw_answer_seconds = 10,  // the time each thread has to answer a stop and a restart
    stw_still_us = 200,       // how long the collector watches the stopped world
    stw_late_seconds = 2,     // how long a late stop handler waits for SIGXCPU to be pending
};

/// What a registered thread does, for the counts of where the stops found it.
enum stw_state { stw_computing, stw_sleeping, stw_waiting, stw_in_java, stw_states };

struct stw_thread {
    pid_t tid;  // 0 once the thread is gone
    pthread_t thread;
    _Atomic unsigned long work;  // counted by the thread itself, never in a handler
    _Atomic int state;
    // Written by the thread's own stop handler.
    unsigned stopped;            // stops that found it
    unsigned found[stw_states];  // the same, by what it did
    long slowest_ns[stw_states]; // its slowest answer, from the stop's first SIGPWR, by what it did
    unsigned late;               // stops in which the handler waited for the restart before its sigsuspend
    unsigned pending;            // the same, where SIGXCPU came and stayed pending, blocked, until sigsuspend
};

static struct {
    pthread_mutex_t lock;        // one stop at a time, as bdwgc's allocation lock; threads come and go under it
    sem_t answers;               // bdwgc's GC_suspend_ack_sem: each handler posts it at its stop and its restart
    sem_t next;                  // a waiter waits on it for the end of the next stop
    _Atomic int world_stopped;   // bdwgc's GC_world_is_stopped
    _Atomic unsigned stop_count; // bdwgc's GC_stop_count
    _Atomic int done;            // the stops are over
    _Atomic long stop_started_ns;  // when the current stop sent its first SIGPWR
    sigset_t wait_mask;          // what sigsuspend blocks: everything but SIGXCPU and the faults
    struct stw_thread threads[stw_max_threads];
    int count;
    unsigned stops;
    unsigned moved;              // stops in which a stopped thread's work count moved
    _Atomic unsigned unblocked;  // handlers that found SIGXCPU unblocked after sigsuspend
    long longest_ns;             // the longest stop, from its first SIGPWR to its last answer
    _Atomic int failed;
    char failure[256];
} stw = {.lock = PTHREAD_MUTEX_INITIALIZER};

static long stw_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

/// Notes the first failure; the later ones follow from it. Returns false, for the callers' returns.
__attribute__((format(printf, 1, 2))) static bool stw_fail(const char *format, ...) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&stw.failed, &expected, 1)) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(stw.failure, sizeof stw.failure, format, arguments);
        va_end(arguments);
    }
    return false;
}

/// The calling thread's entry, or NULL for a thread that is not registered.
static struct stw_thread *stw_self(void) {
    const pid_t tid = gettid();
    for (int i = 0; i < stw.count; i++) {
        if (stw.threads[i].tid == tid) return &stw.threads[i];
    }
    return NULL;
}

/// bdwgc's GC_suspend_handler: answers, then waits in sigsuspend until the world runs again, and answers again.
static void stw_on_stop(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    (void)context;
    const int saved = errno;
    const unsigned stop = atomic_load(&stw.stop_count);
    struct stw_thread *self = stw_self();
    if (self != NULL) {
        const int state = atomic_load(&self->state);
        const long waited = stw_now_ns() - atomic_load(&stw.stop_started_ns);
        self->stopped++;
        self->found[state]++;
        if (waited > self->slowest_ns[state]) self->slowest_ns[state] = waited;
    }
    sem_post(&stw.answers);
    // By turns, the handler reaches sigsuspend only after the restart was sent: SIGXCPU must wait for it, blocked
    // and pending. Unblocked, it would be taken here, and sigsuspend would wait for good.
    if (self != NULL && (stop + (unsigned)(self - stw.threads)) % 4 == 0) {
        while (atomic_load(&stw.world_stopped) && atomic_load(&stw.stop_count) == stop) {
        }
        self->late++;
        const long give_up = stw_now_ns() + stw_late_seconds * 1000000000L;
        sigset_t pending;
        do {
            sigpending(&pending);
        } while (!sigismember(&pending, SIGXCPU) && stw_now_ns() < give_up);
        if (sigismember(&pending, SIGXCPU)) self->pending++;
    }
    do {
        sigsuspend(&stw.wait_mask);
    } while (atomic_load(&stw.world_stopped) && atomic_load(&stw.stop_count) == stop);
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    if (!sigismember(&now, SIGXCPU)) atomic_fetch_add(&stw.unblocked, 1);
    sem_post(&stw.answers);
    errno = saved;
}

/// bdwgc's GC_restart_handler: the signal only ends sigsuspend.
static void stw_on_restart(int signal) {
    (void)signal;
}

/// The semaphores and both handlers, as bdwgc's GC_stop_init sets them up.
static void stw_init(void) {
    sem_init(&stw.answers, 0, 0);
    sem_init(&stw.next, 0, 0);
    sigset_t blocked;
    sigfillset(&blocked);
    const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) sigdelset(&blocked, faults[i]);
    stw.wait_mask = blocked;
    sigdelset(&stw.wait_mask, SIGXCPU);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_mask = blocked;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    action.sa_sigaction = stw_on_stop;
    sigaction(SIGPWR, &action, NULL);
    action.sa_flags = SA_RESTART;
    action.sa_handler = stw_on_restart;
    sigaction(SIGXCPU, &action, NULL);
}

/// Registers the calling thread, as bdwgc's GC_register_my_thread does: the next stop stops it too.
static struct stw_thread *stw_register(void) {
    pthread_mutex_lock(&stw.lock);
    struct stw_thread *self = &stw.threads[stw.count];
    self->tid = gettid();
    self->thread = pthread_self();
    stw.count++;
    pthread_mutex_unlock(&stw.lock);
    return self;
}

static void stw_unregister(struct stw_thread *self) {
    pthread_mutex_lock(&stw.lock);
    self->tid = 0;
    pthread_mutex_unlock(&stw.lock);
}

/// Counts `amount` of work, without a syscall: a thread that spins here must be stopped where it runs.
static void stw_work(struct stw_thread *self, unsigned long amount) {
    atomic_store(&self->state, stw_computing);
    for (unsigned long i = 0; i < amount; i++) atomic_fetch_add_explicit(&self->work, 1, memory_order_relaxed);
}

/// Waits on the semaphore `next` until a stop has ended, blocked in a futex when the next stop comes.
static void stw_wait_for_next_stop(struct stw_thread *self) {
    atomic_store(&self->state, stw_waiting);
    while (sem_wait(&stw.next) != 0) {
    }
    atomic_fetch_add(&self->work, 1);
}

static void stw_sleep_us(struct stw_thread *self, long microseconds) {
    atomic_store(&self->state, stw_sleeping);
    const struct timespec time = {microseconds / 1000000, microseconds % 1000000 * 1000};
    nanosleep(&time, NULL);
    atomic_fetch_add(&self->work, 1);
}

/// Waits until `count` handlers have posted the semaphore, for `what` of stop `stop`.
static bool stw_answered(int count, unsigned stop, const char *what) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += stw_answer_seconds;
    for (int answered = 0; answered < count; answered++) {
        while (sem_timedwait_monotonic_np(&stw.answers, &deadline) != 0) {
            if (errno != EINTR) {
                return stw_fail("%s %u: %d of %d threads answered within %d s", what, stop, answered, count,
                                stw_answer_seconds);
            }
        }
    }
    return true;
}

/// Stops every other registered thread, watches the stopped world and restarts it, with stw.lock held.
static bool stw_collect(const struct stw_thread *self) {
    const unsigned stop = atomic_load(&stw.stop_count) + 1;
    atomic_store(&stw.stop_count, stop);
    atomic_store(&stw.world_stopped, 1);
    const long start = stw_now_ns();
    atomic_store(&stw.stop_started_ns, start);
    int signalled = 0;
    for (int i = 0; i < stw.count; i++) {
        const struct stw_thread *thread = &stw.threads[i];
        if (thread == self || thread->tid == 0) continue;
        const int error = pthread_kill(thread->thread, SIGPWR);
        if (error != 0) return stw_fail("stop %u: pthread_kill: %s", stop, strerror(error));
        signalled++;
    }
    if (!stw_answered(signalled, stop, "stop")) return false;
    const long took = stw_now_ns() - start;
    if (took > stw.longest_ns) stw.longest_ns = took;
    unsigned long before[stw_max_threads];
    for (int i = 0; i < stw.count; i++) before[i] = atomic_load(&stw.threads[i].work);
    const struct timespec still = {0, stw_still_us * 1000L};
    nanosleep(&still, NULL);
    for (int i = 0; i < stw.count; i++) {
        const struct stw_thread *thread = &stw.threads[i];
        if (thread != self && thread->tid != 0 && atomic_load(&thread->work) != before[i]) {
            stw.moved++;
            break;
        }
    }
    atomic_store(&stw.world_stopped, 0);
    for (int i = 0; i < stw.count; i++) {
        const struct stw_thread *thread = &stw.threads[i];
        if (thread != self && thread->tid != 0) pthread_kill(thread->thread, SIGXCPU);
    }
    if (!stw_answered(signalled, stop, "restart")) return false;
    stw.stops++;
    return true;
}

/// Makes a stop unless another thread is making one. Once `minimum` stops are made and `may_end` holds, ends the
/// stops instead, as it does after a failure. False once the stops are over.
static bool stw_try_collect(const struct stw_thread *self, unsigned minimum, bool may_end) {
    if (atomic_load(&stw.done)) return false;
    if (pthread_mutex_trylock(&stw.lock) != 0) return true;
    bool going = !atomic_load(&stw.done);
    if (going && ((stw.stops >= minimum && may_end) || !stw_collect(self))) {
        atomic_store(&stw.done, 1);
        going = false;
    }
    // A waiter's wait ends with the stop; the last one ends it for good.
    int waiting = 0;
    sem_getvalue(&stw.next, &waiting);
    if (waiting == 0 || !going) sem_post(&stw.next);
    pthread_mutex_unlock(&stw.lock);
    return going;
}

/// The sum of the stops' counts over the registered threads, gone ones too.
static unsigned stw_found(enum stw_state state) {
    unsigned sum = 0;
    for (int i = 0; i < stw.count; i++) sum += stw.threads[i].found[state];
    return sum;
}

/// The slowest answer of the registered threads, gone ones too, by what they did.
static long stw_slowest_ns(enum stw_state state) {
    long slowest = 0;
    for (int i = 0; i < stw.count; i++) {
        if (stw.threads[i].slowest_ns[state] > slowest) slowest = stw.threads[i].slowest_ns[state];
    }
    return slowest;
}

static unsigned stw_sum_late(bool pending) {
    unsigned sum = 0;
    for (int i = 0; i < stw.count; i++) sum += pending ? stw.threads[i].pending : stw.threads[i].late;
    return sum;
}

#endif
