// What libdl-lock.so reports to dl-lock-test once the threads it started have ended.
#pragma once

struct dl_lock_report {
    int own_found;              // the constructor's own dlsym, with the lock held, found its symbol
    int handled_while_waiting;  // the waiting thread's handler ran before the lock was free
    int found_in_linker;        // the signal found that thread in the dynamic linker
    int waiter_found;           // its dlsym found its symbol once the lock was free
    int iteration_waited;       // another thread's dl_iterate_phdr waited in the dynamic linker while the lock was held
    int iteration_listed;       // it listed objects once the lock was free
};

/// Waits for the threads the constructor started and fills in `report`.
void dl_lock_finish(struct dl_lock_report *report);
