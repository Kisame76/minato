// C interface of the runtime (loader and Linux syscall layer) for the app.
// Plain C, so Swift can import it as the module APKRuntime (module.modulemap).
#ifndef APKRUNTIME_APKRT_H
#define APKRUNTIME_APKRT_H

#include <stddef.h>

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

/// Runs the program in a signed container dylib the way Linux runs an executable: loads it and,
/// for a dynamic program, the libraries it needs, links them as Android's linker64 does, builds the
/// Linux start stack (argv, envp, auxv) and starts it on a thread of its own. The program ends the
/// whole process through exit_group or with its last thread, with its own exit status, so this
/// returns only when the program could not start; `error` then says why. `argv` and `envp` end with
/// NULL. argv[0] is the program as it was started, e.g. ./name; against the working directory it is
/// the program's guest path, and the containers next to the program's are the library files in
/// that directory. The linker looks for libraries in LD_LIBRARY_PATH of `envp`, then in
/// /system/lib64; one that cannot link the program ends the process as linker64 does.
int apkrt_run_program(const char *container_path, const char *const argv[], const char *const envp[],
                      char *error, size_t error_size);

/// When a guest program ends the process, the runtime first writes its exit status to the file
/// descriptor `descriptor`, as decimal text and a newline: the exit code, or 128 plus the Linux
/// signal number when a signal ends it, as a shell shows it. -1 turns this off. For the app, which
/// cannot wait for its own process: whoever started it reads the status afterwards.
void apkrt_report_exit_status(int descriptor);

#ifdef __cplusplus
}
#endif

#endif
