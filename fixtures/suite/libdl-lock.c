// libdl-lock.so for dl-lock-test: its constructor runs in the main thread's dlopen, which holds the
// dynamic linker's lock (linker64's g_dl_mutex) all along. The constructor takes the lock once more
// with a dlsym of its own, starts a thread whose dlsym has to wait for the lock, and sends that
// thread SIGUSR1 while it waits. A garbage collector that stops threads with signals relies on that.
// A third thread's dl_iterate_phdr waits for the lock as well, as linker64's does.
#define _GNU_SOURCE  // RTLD_DEFAULT
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <ucontext.h>
#include <unistd.h>

#include "libdl-lock.h"

static pthread_t waiter;
static pthread_t iterator;
static atomic_int waiting;         // the waiter is about to call dlsym
static atomic_int handled;         // its handler ran
static const void *interrupted_pc;  // where the signal found it
static void *waiter_symbol;
static void *own_symbol;
static int handled_while_waiting;
static atomic_int iterating;  // the iterator is about to call dl_iterate_phdr
static atomic_int iterated;   // its dl_iterate_phdr returned
static int listed;            // how many objects it listed
static int iteration_waited;

static void on_signal(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    interrupted_pc = (const void *)((ucontext_t *)context)->uc_mcontext.pc;
    atomic_store(&handled, 1);
}

static void *wait_for_the_lock(void *argument) {
    (void)argument;
    atomic_store(&waiting, 1);
    waiter_symbol = dlsym(RTLD_DEFAULT, "printf");
    return NULL;
}

static int count_object(struct dl_phdr_info *info, size_t size, void *data) {
    (void)info;
    (void)size;
    ++*(int *)data;
    return 0;
}

static void *iterate(void *argument) {
    (void)argument;
    int objects = 0;
    atomic_store(&iterating, 1);
    dl_iterate_phdr(count_object, &objects);
    listed = objects;
    atomic_store(&iterated, 1);
    return NULL;
}

__attribute__((constructor)) static void start(void) {
    own_symbol = dlsym(RTLD_DEFAULT, "printf");  // the lock is recursive
    struct sigaction action = {0};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &action, NULL);
    pthread_create(&waiter, NULL, wait_for_the_lock, NULL);
    pthread_create(&iterator, NULL, iterate, NULL);
    while (!atomic_load(&waiting) || !atomic_load(&iterating)) sched_yield();
    usleep(200 * 1000);  // long enough for their calls to reach the lock and wait
    pthread_kill(waiter, SIGUSR1);
    for (int waited = 0; waited < 5000 && !atomic_load(&handled); ++waited) usleep(1000);
    // Its dlsym cannot have returned yet: this thread holds the lock until its dlopen returns.
    handled_while_waiting = atomic_load(&handled);
    iteration_waited = !atomic_load(&iterated);
}

void dl_lock_finish(struct dl_lock_report *report) {
    pthread_join(waiter, NULL);
    pthread_join(iterator, NULL);
    report->own_found = own_symbol != NULL;
    report->handled_while_waiting = handled_while_waiting;
    report->interrupted_pc = interrupted_pc;
    report->waiter_found = waiter_symbol != NULL;
    report->iteration_waited = iteration_waited;
    report->iteration_listed = listed > 0;
}
