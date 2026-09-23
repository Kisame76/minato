// The guest thread pointer in a Darwin TSD slot (ADR-0010, ADR-0019).
//
// XNU rebuilds TPIDR_EL0 from CPU and cluster bits on every context switch, so guest code cannot
// keep its thread pointer there (fact F3, measured with spike S3). The converter rewrites every
// `mrs xN, TPIDR_EL0` into an island that reads the slot below instead. libpthread computes
// `pthread_getspecific(key)` as a load from `TPIDRRO_EL0 + 8 * key` and leaves the slots 6 and 11
// free for code that has the offset built in, which is exactly our case: the offset sits in signed
// code and cannot depend on how many keys Apple's frameworks happened to create first.
#include "APKRuntime/apkrt.h"

#include <stdint.h>

/// Base of the Darwin TSD array of the calling thread. The low bits of TPIDRRO_EL0 are reserved,
/// libsyscall masks them off the same way.
static void **apkrt_tsd_base(void) {
    uint64_t base;
    __asm__("mrs %0, TPIDRRO_EL0" : "=r"(base));
    return (void **)(uintptr_t)(base & ~(uint64_t)7);
}

void apkrt_set_guest_tls(void *thread_pointer) {
    apkrt_tsd_base()[APKRT_GUEST_TLS_SLOT] = thread_pointer;
}

void *apkrt_get_guest_tls(void) {
    return apkrt_tsd_base()[APKRT_GUEST_TLS_SLOT];
}
