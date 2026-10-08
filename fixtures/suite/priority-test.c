// priority-test: per-thread nice values, as Process.setThreadPriority and getThreadPriority use them through
// setpriority and getpriority with PRIO_PROCESS and a thread ID. Each thread has its own value, a new thread starts
// with its creator's, values beyond -20..19 are clamped, and an unknown thread is ESRCH.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t moved = PTHREAD_COND_INITIALIZER;
static int step;
static pid_t worker_tid;

// The nice value of `tid` (0 the caller), or -100 less errno.
static int nice_of(pid_t tid) {
    errno = 0;
    const int value = getpriority(PRIO_PROCESS, (id_t)tid);
    return errno != 0 ? -100 - errno : value;
}

static void wait_for(int wanted) {
    pthread_mutex_lock(&lock);
    while (step < wanted) pthread_cond_wait(&moved, &lock);
    pthread_mutex_unlock(&lock);
}

static void advance(void) {
    pthread_mutex_lock(&lock);
    ++step;
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&lock);
}

static void *worker(void *unused) {
    (void)unused;
    worker_tid = gettid();
    printf("worker at its start: %d\n", nice_of(0));
    printf("worker sets 10: %d\n", setpriority(PRIO_PROCESS, 0, 10));
    printf("worker after its 10: %d\n", nice_of(0));
    advance();
    wait_for(2);
    printf("worker after main's -8: %d\n", nice_of(0));
    advance();
    wait_for(4);  // alive while main asks for the process group
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("main at its start: %d\n", nice_of(0));
    printf("main sets 5: %d\n", setpriority(PRIO_PROCESS, 0, 5));
    printf("main after its 5: %d\n", nice_of(0));
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    wait_for(1);
    printf("main still: %d\n", nice_of(0));
    printf("worker seen from main: %d\n", nice_of(worker_tid));
    printf("main sets the worker's -8: %d\n", setpriority(PRIO_PROCESS, (id_t)worker_tid, -8));
    advance();
    wait_for(3);
    printf("main by the process ID: %d\n", nice_of(getpid()));
    errno = 0;
    const int group = getpriority(PRIO_PGRP, 0);
    printf("the process group's highest: %d\n", errno != 0 ? -100 - errno : group);
    advance();
    const int high = setpriority(PRIO_PROCESS, 0, 100);
    printf("100 clamped: %d, %d\n", high, nice_of(0));
    const int low = setpriority(PRIO_PROCESS, 0, -100);
    printf("-100 clamped: %d, %d\n", low, nice_of(0));
    errno = 0;
    const int unknown = setpriority(PRIO_PROCESS, 999999, 0);
    printf("an unknown thread: %d, %s\n", unknown, strerror(errno));
    errno = 0;
    const int which = getpriority(7, 0);
    printf("which 7: %d, %s\n", which, strerror(errno));
    setpriority(PRIO_PROCESS, 0, 0);
    pthread_join(thread, NULL);
    printf("done\n");
    return 0;
}
