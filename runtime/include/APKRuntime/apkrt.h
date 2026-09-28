// C interface of the runtime (loader and Linux syscall layer) for the app.
// Plain C, so Swift can import it as the module APKRuntime (module.modulemap).
#ifndef APKRUNTIME_APKRT_H
#define APKRUNTIME_APKRT_H

#include <stddef.h>
#include <stdint.h>

// The session's events, configuration and requests, whose layouts the guest shares.
#include "guest_interface.h"

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

/// Called for each request of the guest (ADR-0028, decision 4) on the guest thread that makes it, at once:
/// it must not block (the app hands the request to its main queue). `type` is an enum apkrt_request_type:
/// APKRT_REQUEST_FINISH; APKRT_REQUEST_KEEP_SCREEN_ON with `argument` 1 or 0, only when the flag changes
/// (the runtime keeps the window flags, so APKRT_REQUEST_WINDOW_FLAGS never comes here); or
/// APKRT_REQUEST_START_FAILED with `text`, which lives until the handler returns; or APKRT_REQUEST_FRAME_RATE
/// (ADR-0036, decision 2) with `argument` the bits of a float, the rate in frames per second that
/// ANativeWindow_setFrameRate prefers, 0 for none, when the rate of a window that is not removed changes. The
/// FRAME_RATE handler runs under the window registry's lock, so it must call none of the registry's functions
/// (apkrt_window_*, apkrt_set_background, apkrt_wait_scheduled, apkrt_wait_foreground,
/// apkrt_add_scheduled_provider): then apkrt_window_remove returns only after a report of that window that is
/// under way has returned, and none starts after it. The report names no window, since the host has one at a
/// time (ADR-0028, decision 6), and the host drops that window's preference itself after the removal: a new
/// window has none, as a new Surface on Android. `text` is NULL for all but START_FAILED. The
/// KEEP_SCREEN_ON reports come in the order of the changes, so on and off alternate. The
/// handler runs with the runtime signal blocked, on that thread's stack, which the guest chose and which
/// may be small (an alternate signal stack): a handler that needs more hands its work to another thread.
typedef void (*apkrt_request_handler)(void *context, int type, uint64_t argument, const char *text);

/// How the host describes the app before the program starts (ADR-0028, decision 3).
struct apkrt_session {
    void *context;                             // passed to the handlers
    apkrt_end_handler end;                     // required
    apkrt_request_handler request;             // NULL: the requests go nowhere
    struct apkrt_configuration configuration;  // what activity-host gets when it connects
};

/// Begins the process's session: from now on the program's end calls its end handler instead of ending
/// the process, and the host can post events. Returns 0, or -1 when `session` or its end handler is NULL
/// or a session has begun already.
int apkrt_session_begin(const struct apkrt_session *session);

/// Posts an event to activity-host (ADR-0028, decision 2): a record in the event pipe, which activity-host
/// reads once it runs; `type` is an enum apkrt_event_type, and `a0` to `a2` are its arguments. Returns the
/// event's serial number, from 1, or 0 when it cannot: no session, the program has ended, or the pipe is
/// full because the guest stopped reading. A full pipe is final: every later post fails too, so the guest
/// never sees an event without the ones before it. Posts never block.
uint32_t apkrt_session_post(uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2);

/// Waits until activity-host has answered the event `serial`, that is, its callback returned, at most
/// `seconds` (none below 0 or for NaN; at most 10^9, so INFINITY waits for good). Answers come in the
/// pipe's order, so the answer to a later event counts as well. Returns 1 once answered, 0 when the time is
/// up, -1 when the channel broke or the program has ended, and at once for serial 0 (a failed post) or a
/// serial not posted yet. Never on the main thread (ADR-0032: 2 s for onNativeWindowDestroyed and onDestroy).
int apkrt_session_wait(uint32_t serial, double seconds);

// MARK: - The window, host side (ADR-0028, decision 6)
//
// A registry of the host's windows, whose IDs the guest's ANativeWindow carries. Every function may be
// called from any thread; graphics/host's also from guest threads inside a host call.
//
// The order the app keeps (ADR-0029, decision 15; ADR-0032):
//   - at background: apkrt_set_background(1); apkrt_wait_scheduled (at most 1 s); apkrt_window_remove;
//     only then the events from APKRT_EVENT_PAUSE on;
//   - at Quit and after APKRT_REQUEST_FINISH: apkrt_window_remove, then the events from APKRT_EVENT_PAUSE on
//     (native_app_glue's onPause waits for an app thread that may wait at the closed gate);
//   - at foreground: apkrt_set_background(0), a new window with apkrt_window_add, the events, and the gate
//     opened once the scene is active.

/// For the app: a new window on `layer` (a CAMetalLayer, which the registry retains until the window is
/// removed; NULL in the tests on the Mac), `width` × `height` pixels (the layer's bounds × contentsScale).
/// Returns its ID, from 1, never reused; 0 without memory. Its size stays as it is (M3 fixes the
/// orientation): another size is another window. Its gate starts closed.
uint64_t apkrt_window_add(void *layer, int32_t width, int32_t height);

/// For the app: opens (`open` nonzero) or closes the window's gate. Returns 0, or -1 for a window that is
/// removed or unknown.
int apkrt_window_set_gate(uint64_t id, int open);

/// For the app: the window is gone for good. Its waiters wake and answer "gone", and its layer is released.
/// Returns 0, or -1 for a window that is removed or unknown. A window that comes back is a new ID.
int apkrt_window_remove(uint64_t id);

/// For the app: the window's presents so far, for the FPS label; 0 once it is removed.
uint64_t apkrt_window_presents(uint64_t id);

/// For the app: the process goes into the background (`on` nonzero) or comes back. A sequentially consistent
/// store, which must come before apkrt_wait_scheduled (ADR-0030's count-then-flag, decision 13). Coming back
/// wakes every wait of the background kind and every apkrt_wait_foreground.
void apkrt_set_background(int on);

/// For the app: asks every provider of "scheduled" (apkrt_add_scheduled_provider), one after another on the
/// calling thread, with one deadline `seconds` from now (none below 0 or for NaN; at most 10^9). Returns 1
/// when all of them answered yes in time (also without providers), else 0: a yes after the deadline is late.
int apkrt_wait_scheduled(double seconds);

/// For graphics/host: the window's layer, retained (+1) for the caller, who releases it; NULL once the
/// window is removed, which the app may do while a render thread creates its surface.
void *apkrt_window_layer(uint64_t id);

/// For graphics/host: the window's size in pixels as apkrt_window_add gave it (never the drawable size).
/// Returns 0, or -1 for a window that is removed or unknown.
int apkrt_window_size(uint64_t id, int32_t *width, int32_t *height);

/// For graphics/host: after a present that succeeded, counted for apkrt_window_presents.
void apkrt_window_presented(uint64_t id);

/// For graphics/host: 1 while the process is in the background, else 0; a sequentially consistent load.
int apkrt_is_background(void);

/// For graphics/host: waits until the process is in the foreground, without signals: a wait of host code,
/// for ANGLE's commit gate (ADR-0029, decision 15). Returns at once in the foreground.
void apkrt_wait_foreground(void);

/// A provider of "scheduled": returns 1 once every GPU command its libraries committed so far is scheduled,
/// 0 when `deadline` passes first. `deadline` is in nanoseconds of CLOCK_UPTIME_RAW
/// (clock_gettime_nsec_np), shared by all providers. It runs on the thread of apkrt_wait_scheduled, the
/// main thread, so it must not need the main thread.
typedef int (*apkrt_scheduled_provider)(void *context, uint64_t deadline);

/// For graphics/host: adds a provider of "scheduled", at any time. Returns 0, or -1 when `provider` is NULL
/// or 4 are set already.
int apkrt_add_scheduled_provider(apkrt_scheduled_provider provider, void *context);

// MARK: - The frame clock and touches (ADR-0036, decisions 1 to 3)
//
// For the app: the ticks of its display link and the motion events of its view go to the guest's vsync and
// input channels (guest_interface.h). Every function may be called from any thread; none blocks, and none
// runs guest code.

/// A tick of the display link: its timestamp and targetTimestamp in nanoseconds on CLOCK_UPTIME_RAW, the
/// guest's CLOCK_MONOTONIC. The tick goes to every vsync channel that asked for one since its last, which
/// keeps only the newest. A tick whose period (target_time - time) differs by more than 1 % from the current
/// one makes it the current one and wakes every channel, for the refresh-rate callbacks.
void apkrt_vsync_post(int64_t time, int64_t target_time);

/// A new input channel, whose ID the host sends with APKRT_EVENT_INPUT_QUEUE_CREATED. Returns the ID, from 1,
/// never reused; 0 without memory. The events posted before the guest's queue connects wait in it. Until
/// ADR-0036's step b), it returns 0.
uint64_t apkrt_input_add(void);

/// Posts a motion event (struct apkrt_motion_event) to the input channel `id`, after those before it.
/// `pointer_ids` holds `pointer_count` IDs, 1 to APKRT_MOTION_POINTERS of them, ascending; `sample_times`
/// holds `sample_count` times, the history's oldest first and the event's own last; `coords` holds
/// sample_count × pointer_count places, by sample, then pointer index. Of more than APKRT_MOTION_SAMPLES
/// samples, the oldest go. Returns 1 when the channel took the event, else 0: an unknown ID, a channel whose
/// queue has closed, an event that is not sound, or a full channel (256 events) in which no MOVE could merge.
/// A full channel merges MOVEs: a MOVE's samples become the history of the MOVE right after it with the same
/// pointers. Until ADR-0036's step b), it returns 0.
int apkrt_input_post(uint64_t id, int32_t action, int64_t down_time, uint32_t pointer_count,
                     const int32_t *pointer_ids, uint32_t sample_count, const int64_t *sample_times,
                     const struct apkrt_pointer_coords *coords);

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
