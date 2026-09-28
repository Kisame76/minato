/* minato_gles_proc (libgles.h): tools/frontgen's list of the table's names, each with the function this lib
   hands out for it. Without the GL headers: every function is declared alike here, as the list names it. */
#include <stdlib.h>
#include <string.h>

void *minato_gles_proc(const char *name);

#define MINATO_PROC(name, function) extern void function(void);
#include "gles_procs.inc"
#undef MINATO_PROC

struct proc {
    const char *name;
    void (*function)(void);
};
#define MINATO_PROC(name, function) {#name, function},
static const struct proc procs[] = {
#include "gles_procs.inc"
};
#undef MINATO_PROC

static int by_name(const void *key, const void *element) {
    return strcmp(key, ((const struct proc *)element)->name);
}

void *minato_gles_proc(const char *name) {
    const struct proc *proc =
        name != NULL ? bsearch(name, procs, sizeof procs / sizeof *procs, sizeof *procs, by_name) : NULL;
    return proc != NULL ? (void *)proc->function : NULL;
}
