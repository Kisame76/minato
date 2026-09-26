/* The binding of a front lib's host table (ADR-0030, decision 3); see minato_bind.h. */
#include <errno.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "minato_bind.h"

__attribute__((naked)) void minato_no_driver(void) {
    __asm__("mov x0, x16\n\t"
            "fmov d0, xzr\n\t"
            "ret");
}

int minato_bind_host_table(const char *name, void **table, size_t length, uint64_t hash, const int64_t *no_driver) {
    if (syscall(APKRT_NR_BIND_HOST_TABLE, name, table, length, hash) == 0) {
        return 0;
    }
    const int error = -errno;
    /* Plain stores: the lib's first constructor binds once, before any stub of the lib runs. */
    table[0] = (void *)minato_no_driver;
    for (size_t slot = 1; slot < length; slot++) {
        table[slot] = (void *)(intptr_t)no_driver[slot];
    }
    return error;
}
