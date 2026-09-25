// dl-lock-test: the dynamic linker's lock against signals (ADR-0025, addition d). A thread whose
// dlsym waits for the lock, because a constructor in another thread's dlopen holds it, still takes
// signals, and its handler finds it in the dynamic linker's own code: linker64 on Android,
// ld-android.so here. dl_iterate_phdr waits for the lock too. libdl-lock.so does the work in its
// constructor.
// Expected output: fixtures/expected/dl-lock-test.out
#define _GNU_SOURCE  // RTLD_DEFAULT
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>

#include "libdl-lock.h"

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
    printf("the signal found it in the dynamic linker: %s\n", report.found_in_linker ? "yes" : "no");
    printf("its dlsym after the dlopen: %s\n", report.waiter_found ? "found" : "NULL");
    printf("dl_iterate_phdr in a third thread waited for the dlopen: %s, then listed objects: %s\n",
           report.iteration_waited ? "yes" : "no", report.iteration_listed ? "yes" : "no");
    return 0;
}
