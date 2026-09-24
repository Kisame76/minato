// dl-lock-test: the dynamic linker's lock against signals (ADR-0025, addition d). A thread whose
// dlsym waits for the lock, because a constructor in another thread's dlopen holds it, still takes
// signals, and its handler finds it in the dynamic linker's own code: linker64 on Android,
// ld-android.so here. dl_iterate_phdr waits for the lock too. libdl-lock.so does the work in its
// constructor.
// Expected output: fixtures/expected/dl-lock-test.out
#define _GNU_SOURCE  // RTLD_DEFAULT
#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include <stdio.h>

#include "libdl-lock.h"

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
/// not know linker64's addresses, while its dl_iterate_phdr lists linker64.
static int same_object(const void *first, const void *second) {
    struct Pair pair = {first, second, 0};
    if (first != NULL && second != NULL) dl_iterate_phdr(look_at, &pair);
    return pair.together;
}

int main(void) {
    void *library = dlopen("libdl-lock.so", RTLD_NOW);
    printf("dlopen: %s\n", library != NULL ? "a handle" : dlerror());
    if (library == NULL) return 1;
    void (*finish)(struct dl_lock_report *) = (void (*)(struct dl_lock_report *))dlsym(library, "dl_lock_finish");
    struct dl_lock_report report = {0};
    finish(&report);
    printf("the constructor's own dlsym, with the lock held: %s\n", report.own_found ? "found" : "NULL");
    printf("the other thread's dlsym took SIGUSR1 while it waited for the lock: %s\n",
           report.handled_while_waiting ? "yes" : "no");
    // The dynamic linker is the object that serves libdl.
    const void *loader = dlsym(RTLD_DEFAULT, "__loader_dlsym");
    printf("the signal found it in the dynamic linker: %s\n", same_object(report.interrupted_pc, loader) ? "yes" : "no");
    printf("its dlsym after the dlopen: %s\n", report.waiter_found ? "found" : "NULL");
    printf("dl_iterate_phdr in a third thread waited for the dlopen: %s, then listed objects: %s\n",
           report.iteration_waited ? "yes" : "no", report.iteration_listed ? "yes" : "no");
    return 0;
}
