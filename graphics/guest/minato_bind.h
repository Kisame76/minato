/* The binding of a front lib's host table (ADR-0030, decision 3), guest code that every front lib shares.
   tools/frontgen's <lib>_bind.c calls it from the lib's first constructor. */
#ifndef MINATO_BIND_H
#define MINATO_BIND_H

#include <stddef.h>
#include <stdint.h>

#define MINATO_HIDDEN __attribute__((visibility("hidden")))

/* Binds `table` through the runtime's syscall "bind host table" (APKRT_NR_BIND_HOST_TABLE) and returns 0
   once graphics/host's provider filled it and the runtime put its entry into slot 0. Otherwise it returns
   the syscall's negative error and the lib has no driver: slot 0 gets minato_no_driver and slot n the
   answer no_driver[n] itself, which minato_no_driver hands back to every stub of slot n. */
MINATO_HIDDEN int minato_bind_host_table(const char *name, void **table, size_t length, uint64_t hash,
                                         const int64_t *no_driver);

/* Slot 0 of a table without a driver. A stub reaches it with its slot's content in x16, which is the
   answer here: it returns x16 in x0 and 0.0 in v0, for a floating-point result. */
MINATO_HIDDEN void minato_no_driver(void);

#endif
