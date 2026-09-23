// thread-test: POSIX threads with bionic (M2 step 5c): create and join with results, mutexes of
// every type, condition variables with and without timeouts, once, keys with destructors, detached
// threads, read-write locks, barriers, semaphores, thread IDs, names, stack sizes, errno and
// thread-local variables per thread. Every line is a count or a fixed answer, whatever the timing.
// Expected output: fixtures/expected/thread-test.out
#define _GNU_SOURCE  // pthread_getname_np
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { workers = 8, increments = 20000 };

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *add_many(void *argument) {
    (void)argument;
    for (int i = 0; i < increments; ++i) {
        pthread_mutex_lock(&counter_lock);
        ++counter;
        pthread_mutex_unlock(&counter_lock);
    }
    return NULL;
}

static void *sum_range(void *argument) {
    const intptr_t start = (intptr_t)argument;
    intptr_t sum = 0;
    for (intptr_t i = start; i < start + 1000; ++i) sum += i;
    if (start == 3000) pthread_exit((void *)sum);  // the same as returning
    return (void *)sum;
}

static void results_and_mutexes(void) {
    pthread_t threads[4];
    for (intptr_t i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, sum_range, (void *)(i * 1000));
    intptr_t total = 0;
    for (int i = 0; i < 4; ++i) {
        void *result;
        pthread_join(threads[i], &result);
        total += (intptr_t)result;
    }
    printf("join results: %ld\n", (long)total);

    pthread_t adders[workers];
    for (int i = 0; i < workers; ++i) pthread_create(&adders[i], NULL, add_many, NULL);
    for (int i = 0; i < workers; ++i) pthread_join(adders[i], NULL);
    printf("mutex counter: %ld\n", counter);

    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t recursive;
    pthread_mutex_init(&recursive, &attributes);
    const int first_lock = pthread_mutex_lock(&recursive);
    const int second_lock = pthread_mutex_lock(&recursive);
    const int first_unlock = pthread_mutex_unlock(&recursive);
    const int second_unlock = pthread_mutex_unlock(&recursive);
    printf("recursive: %d %d %d %d\n", first_lock, second_lock, first_unlock, second_unlock);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t checked;
    pthread_mutex_init(&checked, &attributes);
    pthread_mutex_lock(&checked);
    const int relock = pthread_mutex_lock(&checked);
    const int trylock = pthread_mutex_trylock(&checked);
    printf("errorcheck relock: %s, trylock: %s\n", strerror(relock), strerror(trylock));
    struct timespec soon;
    clock_gettime(CLOCK_REALTIME, &soon);
    soon.tv_nsec += 20 * 1000 * 1000;
    if (soon.tv_nsec >= 1000000000) {
        soon.tv_sec += 1;
        soon.tv_nsec -= 1000000000;
    }
    pthread_mutex_t plain = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&plain);
    printf("timedlock on a held mutex from the same thread: %s\n", strerror(pthread_mutex_timedlock(&plain, &soon)));
    pthread_mutex_unlock(&plain);
    pthread_mutex_unlock(&checked);
}

// Producer and consumer through a bounded queue.
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
static int queue[4];
static int queued, head;

static void *produce(void *argument) {
    (void)argument;
    for (int value = 0; value < 100; ++value) {
        pthread_mutex_lock(&queue_lock);
        while (queued == 4) pthread_cond_wait(&queue_changed, &queue_lock);
        queue[(head + queued) % 4] = value;
        ++queued;
        pthread_cond_broadcast(&queue_changed);
        pthread_mutex_unlock(&queue_lock);
    }
    return NULL;
}

static void condition_variables(void) {
    pthread_t producer;
    pthread_create(&producer, NULL, produce, NULL);
    long sum = 0;
    for (int received = 0; received < 100; ++received) {
        pthread_mutex_lock(&queue_lock);
        while (queued == 0) pthread_cond_wait(&queue_changed, &queue_lock);
        sum += queue[head];
        head = (head + 1) % 4;
        --queued;
        pthread_cond_broadcast(&queue_changed);
        pthread_mutex_unlock(&queue_lock);
    }
    pthread_join(producer, NULL);
    printf("condition variable queue: sum %ld\n", sum);

    struct timespec before, deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    clock_gettime(CLOCK_MONOTONIC, &before);
    deadline.tv_nsec += 30 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&queue_lock);
    const int waited = pthread_cond_timedwait(&queue_changed, &queue_lock, &deadline);
    pthread_mutex_unlock(&queue_lock);
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    printf("cond timedwait: %s after at least 25 ms: %s\n", strerror(waited), elapsed_ms >= 25 ? "yes" : "no");
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int initialized;
static void initialize(void) {
    ++initialized;
}
static void *call_once(void *argument) {
    (void)argument;
    pthread_once(&once, initialize);
    return NULL;
}

static pthread_key_t key;
static int destructed;
static pthread_mutex_t destructed_lock = PTHREAD_MUTEX_INITIALIZER;
static void destruct(void *value) {
    pthread_mutex_lock(&destructed_lock);
    destructed += (int)(intptr_t)value;
    pthread_mutex_unlock(&destructed_lock);
}
static void *use_key(void *argument) {
    pthread_setspecific(key, argument);
    return pthread_getspecific(key);
}

static void once_and_keys(void) {
    pthread_t threads[workers];
    for (int i = 0; i < workers; ++i) pthread_create(&threads[i], NULL, call_once, NULL);
    for (int i = 0; i < workers; ++i) pthread_join(threads[i], NULL);
    printf("pthread_once: initialized %d time(s)\n", initialized);

    pthread_key_create(&key, destruct);
    intptr_t seen = 0;
    for (intptr_t i = 1; i <= 4; ++i) {
        void *result;
        pthread_create(&threads[i], NULL, use_key, (void *)i);
        pthread_join(threads[i], &result);
        seen += (intptr_t)result;
    }
    printf("keys: each thread saw its own value (sum %ld), destructors summed %d, main sees %s\n", (long)seen,
           destructed, pthread_getspecific(key) == NULL ? "NULL" : "a value");
    pthread_key_delete(key);
}

static sem_t detached_done;
static void *detached(void *argument) {
    (void)argument;
    sem_post(&detached_done);
    return NULL;
}

static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static int shared_value;
static void *read_value(void *argument) {
    pthread_rwlock_rdlock(&rwlock);
    *(int *)argument = shared_value;
    pthread_rwlock_unlock(&rwlock);
    return NULL;
}

static pthread_barrier_t barrier;
static int serial;
static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;
static void *wait_at_barrier(void *argument) {
    (void)argument;
    if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
        pthread_mutex_lock(&serial_lock);
        ++serial;
        pthread_mutex_unlock(&serial_lock);
    }
    return NULL;
}

static void other_primitives(void) {
    sem_init(&detached_done, 0, 0);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    for (int i = 0; i < 4; ++i) pthread_create(&thread, &attributes, detached, NULL);
    for (int i = 0; i < 4; ++i) sem_wait(&detached_done);
    printf("detached threads posted: 4\n");
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    now.tv_nsec += 10 * 1000 * 1000;
    if (now.tv_nsec >= 1000000000) {
        now.tv_sec += 1;
        now.tv_nsec -= 1000000000;
    }
    const int timed = sem_timedwait(&detached_done, &now);
    printf("sem_timedwait on an empty semaphore: %d %s\n", timed, strerror(errno));

    pthread_rwlock_wrlock(&rwlock);
    shared_value = 42;
    int readings[4];
    pthread_t readers[4];
    for (int i = 0; i < 4; ++i) pthread_create(&readers[i], NULL, read_value, &readings[i]);
    pthread_rwlock_unlock(&rwlock);
    for (int i = 0; i < 4; ++i) pthread_join(readers[i], NULL);
    printf("rwlock readers saw: %d %d %d %d\n", readings[0], readings[1], readings[2], readings[3]);

    pthread_barrier_init(&barrier, NULL, 4);
    pthread_t waiters[4];
    for (int i = 0; i < 4; ++i) pthread_create(&waiters[i], NULL, wait_at_barrier, NULL);
    for (int i = 0; i < 4; ++i) pthread_join(waiters[i], NULL);
    printf("barrier: %d serial thread\n", serial);
    pthread_barrier_destroy(&barrier);
}

static __thread int per_thread = 7;
static pid_t main_tid;

struct Identity {
    pid_t tid;
    int errno_before;
    int per_thread_before;
    char name[16];
    size_t stack_size;
};

static void *identify(void *argument) {
    struct Identity *identity = argument;
    identity->tid = gettid();
    identity->errno_before = errno;
    errno = 42;
    identity->per_thread_before = per_thread;
    per_thread = 100;
    pthread_setname_np(pthread_self(), "worker");
    pthread_getname_np(pthread_self(), identity->name, sizeof identity->name);
    pthread_attr_t attributes;
    pthread_getattr_np(pthread_self(), &attributes);
    pthread_attr_getstacksize(&attributes, &identity->stack_size);
    return NULL;
}

static void identities(void) {
    main_tid = gettid();
    printf("main thread: tid is the pid: %s\n", main_tid == getpid() ? "yes" : "no");
    errno = 7;
    per_thread = 8;
    struct Identity identity;
    memset(&identity, 0, sizeof identity);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    size_t default_size;
    pthread_attr_getstacksize(&attributes, &default_size);
    printf("default stack size: %zu\n", default_size);
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    pthread_t thread;
    pthread_create(&thread, &attributes, identify, &identity);
    pthread_join(thread, NULL);
    printf("thread: own tid %s, errno starts at %d, __thread starts at %d, name %s, stack at least 256 KiB: %s\n",
           identity.tid != main_tid && identity.tid > 0 ? "yes" : "no", identity.errno_before,
           identity.per_thread_before, identity.name, identity.stack_size >= 256 * 1024 ? "yes" : "no");
    printf("main after the thread: errno %d, __thread %d, same thread: %d\n", errno, per_thread,
           pthread_equal(pthread_self(), thread) != 0);
}

static void *nothing(void *argument) {
    return argument;
}

static void many(void) {
    enum { count = 64 };
    pthread_t threads[count];
    intptr_t sum = 0;
    for (intptr_t round = 0; round < 4; ++round) {
        for (intptr_t i = 0; i < count; ++i) pthread_create(&threads[i], NULL, nothing, (void *)i);
        for (int i = 0; i < count; ++i) {
            void *result;
            pthread_join(threads[i], &result);
            sum += (intptr_t)result;
        }
    }
    printf("256 threads created and joined, results %ld\n", (long)sum);
}

int main(void) {
    results_and_mutexes();
    condition_variables();
    once_and_keys();
    other_primitives();
    identities();
    many();
    return 0;
}
