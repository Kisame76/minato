// C interface of the runtime (loader and Linux syscall layer) for the app.
// Plain C, so Swift can import it as the module APKRuntime (module.modulemap).
#ifndef APKRUNTIME_APKRT_H
#define APKRUNTIME_APKRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Version of the runtime, e.g. "0.1.0".
const char *apkrt_version(void);

// MARK: - Guest thread pointer (ADR-0010, ADR-0019)

/// The Darwin TSD slot that holds the guest thread pointer. XNU overwrites TPIDR_EL0 on every
/// context switch, so converted guest code reads its thread pointer from this slot instead.
/// libpthread leaves the slots 6 and 11 free for code with a hard-coded offset, which is what the
/// islands use: `mrs xN, TPIDRRO_EL0; ldr xN, [xN, #APKRT_GUEST_TLS_OFFSET]`.
#define APKRT_GUEST_TLS_SLOT 6
#define APKRT_GUEST_TLS_OFFSET (8 * APKRT_GUEST_TLS_SLOT)

/// Sets the guest thread pointer of the calling thread. Every thread that runs guest code needs
/// one, otherwise the guest's stack protector reads from a null pointer.
void apkrt_set_guest_tls(void *thread_pointer);

/// The guest thread pointer of the calling thread, or NULL when the thread has none.
void *apkrt_get_guest_tls(void);

// MARK: - Guest x18 (ADR-0020)

/// The Darwin TSD slot that holds the guest's x18. XNU sets x18 to zero at every context switch,
/// so converted guest code runs each use of x18 in an island that keeps the value in this slot:
/// `mrs xN, TPIDRRO_EL0; bic xN, xN, #7; ldr xN, [xN, #APKRT_GUEST_X18_OFFSET]`.
#define APKRT_GUEST_X18_SLOT 11
#define APKRT_GUEST_X18_OFFSET (8 * APKRT_GUEST_X18_SLOT)

/// Sets the guest x18 of the calling thread.
void apkrt_set_guest_x18(uint64_t value);

/// The guest x18 of the calling thread; 0 in a thread that has not run guest code.
uint64_t apkrt_get_guest_x18(void);

// MARK: - Guest file system (ARCHITECTURE.md 4.5)

/// Puts the host directory or file `host_path` under the guest path `guest_path` (absolute), for
/// every program this process runs; the deepest mount covering a path wins. /dev/null, /dev/zero,
/// /dev/random and /dev/urandom are always there. Returns 0, or -1 with the reason in `error`.
int apkrt_mount(const char *guest_path, const char *host_path, char *error, size_t error_size);

/// The guest's working directory at start, an absolute guest path; "/" unless set.
int apkrt_set_working_directory(const char *guest_path, char *error, size_t error_size);

// MARK: - Programs (ADR-0019, ADR-0021)

/// The signed library containers in the host directory `host_path` (<file name>.dylib, as
/// apk-convert writes them) are the library files the linker finds in the guest directory
/// `guest_directory` (absolute), for every program this process runs; e.g. /system/lib64 for the
/// system's libraries. Returns 0, or -1 with the reason in `error`.
int apkrt_add_library_directory(const char *guest_directory, const char *host_path, char *error, size_t error_size);

/// Starts the program in a signed container dylib the way Linux runs an executable, on a thread of its
/// own, which becomes the program's first guest thread (ADR-0028, decision 5). The calling thread maps
/// the program's container and builds the Linux start stack (argv, envp, auxv); the program's thread
/// loads the libraries a dynamic program needs, links them as Android's linker64 does, runs their ifunc
/// resolvers and constructors, and then the program. So no guest code runs on the calling thread, which
/// gets its thread back: 0 once the program runs, or -1 when it cannot start; `error` then says why. The
/// program ends through exit_group or with its last thread, with its own exit status: with a session
/// (apkrt_session_begin) its end handler is called, otherwise the process ends. One program per
/// process: every call after the first returns -1. `argv` and `envp` end with NULL. argv[0] is the program as it was started, e.g. ./name;
/// against the working directory it is the program's guest path, and the containers next to the
/// program's are the library files in that directory. The linker looks for libraries in LD_LIBRARY_PATH
/// of `envp`, then in /system/lib64; one that cannot link the program ends it as linker64 does, with
/// exit status 1.
int apkrt_start_program(const char *container_path, const char *const argv[], const char *const envp[],
                        char *error, size_t error_size);

/// apkrt_start_program, and then waits for good: returns only when the program could not start.
int apkrt_run_program(const char *container_path, const char *const argv[], const char *const envp[],
                      char *error, size_t error_size);

/// When a guest program ends the process, the runtime first writes its exit status to the file
/// descriptor `descriptor`, as decimal text and a newline: the exit code, or 128 plus the Linux
/// signal number when a signal ends it, as a shell shows it. -1 turns this off. For the app, which
/// cannot wait for its own process: whoever started it reads the status afterwards.
void apkrt_report_exit_status(int descriptor);

// MARK: - Session (ADR-0028)

/// Called at the program's end (ADR-0028, decision 5), once, on the thread that ends it (exit_group, or
/// its last thread, or linker64's failure), after the exit status went to the report descriptor and
/// every other guest thread was told to stop: each stops on its next way back into guest code, one
/// inside a host call once that call returns, so they may still finish host code meanwhile. `status` is
/// the exit status as reported. A stopped thread may hold locks, the
/// loader's or those of ANGLE and MoltenVK: so the handler uses neither the loader nor the graphics
/// libraries, and ends the process with _exit, never with exit, whose destructors would meet the
/// stopped threads. It runs on the stack that thread had, which the guest chose and which may be small
/// (an alternate signal stack): a handler that needs more hands its work to another thread. If it
/// returns, its thread sleeps for good. A signal that ends the program (a crash) ends the process
/// without it, after the report of 128 + the signal.
typedef void (*apkrt_end_handler)(void *context, int status);

/// How the host describes the app before the program starts (ADR-0028, decision 3). The configuration
/// and the handler for the guest's requests come with ADR-0028's step c.
struct apkrt_session {
    void *context;          // passed to the handlers
    apkrt_end_handler end;  // required
};

/// Begins the process's session: from now on the program's end calls its end handler instead of ending
/// the process. Returns 0, or -1 when `session` or its end handler is NULL or a session has begun
/// already.
int apkrt_session_begin(const struct apkrt_session *session);

// MARK: - Host code on guest threads (ADR-0030)

/// 1 when the calling thread is a guest thread, else 0.
int apkrt_is_guest_thread(void);

/// Calls the guest function at `function` with up to eight integer or pointer arguments and returns
/// its x0. On a guest thread the function runs as guest code, and the caller's side (host code, or
/// the runtime's own code) holds again once it returns (ADR-0025, decision 5; ADR-0030, A2). On a
/// host thread it is a plain call. graphics/host calls the app's callbacks this way, on guest
/// threads only (ADR-0030, decision 5).
uint64_t apkrt_call_guest(uint64_t function, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6, uint64_t a7);

/// Fills a front lib's host table (ADR-0030, decision 3): slot n, from 1 to `length` - 1, gets the
/// host function of the n-th name in tools/frontgen's list for the lib. Returns 0, or nonzero when it
/// cannot: a `hash` or `length` other than its list's, a host library that does not load. Slot 0 is
/// the runtime's entry, which the runtime writes after a provider succeeded. The provider runs on the
/// guest thread whose front lib binds its table, as host code: no runtime signal interrupts it.
typedef int (*apkrt_host_provider)(void *context, void **table, size_t length, uint64_t hash);

/// Sets the provider of the front lib `name` (tools/frontgen's names: "vulkan", "gles", "egl"), before
/// the program starts; a NULL `provider` removes it. Returns 0, or -1 when `name` is empty or longer
/// than 63 bytes, or 8 providers are set already.
int apkrt_set_host_provider(const char *name, apkrt_host_provider provider, void *context);

#ifdef __cplusplus
}
#endif

#endif
