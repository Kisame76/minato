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
static atomic_int waiting;    // the waiter is about to call dlsym
static atomic_int looked_up;  // its dlsym returned
static atomic_int iterating;  // the iterator is about to call dl_iterate_phdr
static atomic_int iterated;   // its dl_iterate_phdr returned
static atomic_int handled;    // the handler ran
static const void *interrupted_pc;  // where the signal found its thread
static void *waiter_symbol;
static void *own_symbol;
static int listed;  // how many objects the iterator listed
static struct dl_lock_report result;

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
    atomic_store(&looked_up, 1);
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

struct Pair {
    const void *first;
    const void *second;
    int together;
};

static int contains(const struct dl_phdr_info *info, const void *address) {
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[index];
        const ElfW(Addr) start = info->dlpi_addr + phdr->p_vaddr;
        if (phdr->p_type == PT_LOAD && (ElfW(Addr))address >= start && (ElfW(Addr))address - start < phdr->p_memsz) {
            return 1;
        }
    }
    return 0;
}

static int look_at(struct dl_phdr_info *info, size_t size, void *argument) {
    struct Pair *pair = argument;
    (void)size;
    if (contains(info, pair->first) && contains(info, pair->second)) pair->together = 1;
    return 0;
}

/// Whether both addresses lie in the loaded segments of one object. Not with dladdr: Android's does
/// not know linker64's addresses, while its dl_iterate_phdr lists linker64. The lock is recursive,
/// so the constructor may call it.
static int same_object(const void *first, const void *second) {
    struct Pair pair = {first, second, 0};
    if (first != NULL && second != NULL) dl_iterate_phdr(look_at, &pair);
    return pair.together;
}

/// Sends `thread` SIGUSR1 until its handler finds it in the dynamic linker, the object that serves
/// libdl, for about 5 s at most: the thread may not have reached the lock yet. Sets `*ran` once the
/// handler has run at all.
static int found_in_the_dynamic_linker(pthread_t thread, int *ran) {
    const void *loader = dlsym(RTLD_DEFAULT, "__loader_dlsym");
    for (int round = 0; round < 50; ++round) {
        atomic_store(&handled, 0);
        if (pthread_kill(thread, SIGUSR1) != 0) return 0;
        for (int waited = 0; waited < 1000 && !atomic_load(&handled); ++waited) usleep(1000);
        if (!atomic_load(&handled)) return 0;
        *ran = 1;
        if (same_object(interrupted_pc, loader)) return 1;
        usleep(100 * 1000);
    }
    return 0;
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
    int handled_in_iterator = 0;
    result.found_in_linker = found_in_the_dynamic_linker(waiter, &result.handled_while_waiting);
    const int iterator_in_linker = found_in_the_dynamic_linker(iterator, &handled_in_iterator);
    usleep(100 * 1000);  // time enough for a call that does not wait to return
    // Neither call can have returned yet: this thread holds the lock until its dlopen returns.
    result.handled_while_waiting = result.handled_while_waiting && !atomic_load(&looked_up);
    result.iteration_waited = iterator_in_linker && !atomic_load(&iterated);
}

void dl_lock_finish(struct dl_lock_report *report) {
    pthread_join(waiter, NULL);
    pthread_join(iterator, NULL);
    *report = result;
    report->own_found = own_symbol != NULL;
    report->waiter_found = waiter_symbol != NULL;
    report->iteration_listed = listed > 0;
}
