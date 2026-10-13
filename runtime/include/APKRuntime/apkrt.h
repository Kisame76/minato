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

/// When a guest program ends the process, also by a signal, the runtime writes what the run measured to the file
/// descriptor `descriptor`, after the exit status (apkrt_report_exit_status): a "<name> <value>\n" line each, in
/// this order,
///   run-ms              milliseconds from apkrt_start_program (or apkrt_run_program) to the end, one decimal
///   process-ms          milliseconds from the process's start to the end, one decimal
///   footprint-peak      the peak of the physical footprint, which iOS's memory limit counts, in bytes
///   resident-peak       the peak resident size in bytes
///   virtual             the virtual size at the end in bytes
///   available-at-start  iOS only: the bytes os_proc_available_memory gave at the program's start
/// and nothing when no program started. -1 turns this off. For the benchmarks: whoever started the process reads
/// the measures afterwards, as the status.
void apkrt_report_exit_measures(int descriptor);

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
/// window has none, as a new Surface on Android. APKRT_REQUEST_WINDOW_OPAQUE comes with `argument` 1 or 0 when
/// the window's opacity changes: the runtime keeps the window's format, so APKRT_REQUEST_WINDOW_FORMAT never
/// comes here, and the session starts opaque without a report. `text` is NULL for all but START_FAILED. The
/// KEEP_SCREEN_ON reports come in the order of the changes, so on and off alternate, and so do the
/// WINDOW_OPAQUE reports. The
/// handler runs with the runtime signal blocked, on that thread's stack, which the guest chose and which
/// may be small (an alternate signal stack): a handler that needs more hands its work to another thread.
typedef void (*apkrt_request_handler)(void *context, int type, uint64_t argument, const char *text);

/// Called for each surface request of the guest (ADR-0043 c4, guest_interface.h's struct apkrt_surface_request), on
/// the guest thread that makes it, at once, with the runtime signal blocked, as the request handler is: it must not
/// block. `request` lives until the handler returns. The runtime has checked it: per token a make, places and a
/// remove, in that order, at most 16 tokens live; for a make and a place the buffer's size is set (the frame's when
/// the guest gave 0 and 0) and so is `opaque`, and a place carries the make's format; a remove carries only the op and
/// the token. The handler runs under the runtime's surface lock, so requests reach the host in the order the runtime
/// took them, from whichever threads. The host answers, in its own order:
///   - a make with a window of its own (apkrt_window_add) on a layer at the frame, below the window's layer for a
///     negative z and above it for a positive one, by z and then in the order made, then APKRT_EVENT_SURFACE_CREATED;
///     its gate opens as the window's does, so not in the background;
///   - a place with the new frame and z, and for another size apkrt_window_set_size, then
///     APKRT_EVENT_SURFACE_RESIZED; no event for a move;
///   - a remove with apkrt_window_remove, without an event;
///   - at the background and the end it removes every surface's window with the window, before
///     APKRT_EVENT_PAUSE, and posts APKRT_EVENT_SURFACE_DESTROYED for each after PAUSE and the focus, before
///     NATIVE_WINDOW_DESTROYED and STOP, where Android 15 calls surfaceDestroyed from the stopped window. The guest's
///     place and remove of such a surface still come, and the host ignores them.
typedef void (*apkrt_surface_handler)(void *context, const struct apkrt_surface_request *request);

/// Called for each window request of the guest (ADR-0041, gap 4, guest_interface.h's struct apkrt_window_request), as
/// the surface handler is for surfaces, under the runtime's window lock: per token a make, places and a remove, at
/// most 16 tokens live; for a make and a place the buffer's size is set and so is `opaque`, and a place carries the
/// make's type, parent, group and format; a remove carries only the op and the token, and a sub-window's comes
/// before its parent's. The host answers, in its own order:
///   - a make with a window of its own (apkrt_window_add) on a layer in ADR-0041's order of windows, then
///     APKRT_EVENT_WINDOW_CREATED; its gate opens as the window's does, so not in the background;
///   - a place with the new frame, alpha and dim, and for another size apkrt_window_set_size, then
///     APKRT_EVENT_WINDOW_RESIZED; no event otherwise;
///   - a remove with apkrt_window_remove, without an event;
///   - at the background and the end it removes every requested window with the window, before APKRT_EVENT_PAUSE,
///     and posts APKRT_EVENT_WINDOW_DESTROYED for each after PAUSE and the focus, before NATIVE_WINDOW_DESTROYED and
///     STOP, a sub-window's before its parent's. The guest's place and remove of such a window still come, and the
///     host ignores them.
typedef void (*apkrt_window_handler)(void *context, const struct apkrt_window_request *request);

/// How the host describes the app before the program starts (ADR-0028, decision 3).
struct apkrt_session {
    void *context;                             // passed to the handlers
    apkrt_end_handler end;                     // required
    apkrt_request_handler request;             // NULL: the requests go nowhere
    struct apkrt_configuration configuration;  // what activity-host gets when it connects
    apkrt_surface_handler surface;             // NULL: surface requests answer -EOPNOTSUPP (ADR-0043 c4)
    apkrt_window_handler window;               // NULL: window requests answer -EOPNOTSUPP (ADR-0041, gap 4)
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
/// Returns its ID, from 1, never reused; 0 without memory. Its size changes only through
/// apkrt_window_set_size. Its gate starts closed.
uint64_t apkrt_window_add(void *layer, int32_t width, int32_t height);

/// For the app: opens (`open` nonzero) or closes the window's gate. Returns 0, or -1 for a window that is
/// removed or unknown.
int apkrt_window_set_gate(uint64_t id, int open);

/// For the app, at a turn (ADR-0038, decision 1.4): the window keeps its ID and takes a new size in pixels. From
/// the return on, apkrt_window_size and the guest's ANativeWindow_getWidth and getHeight answer it; the size of
/// ANativeWindow_setBuffersGeometry stays apart, as before (ADR-0028 b). It posts no event: the host posts
/// APKRT_EVENT_CONFIGURATION_CHANGED before the call, and NATIVE_WINDOW_RESIZED, CONTENT_RECT_CHANGED and
/// NATIVE_WINDOW_REDRAW_NEEDED after it. Returns 0, or -1 for a window that is removed or unknown or a size not
/// above 0.
int apkrt_window_set_size(uint64_t id, int32_t width, int32_t height);

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

/// For graphics/host: the window's size in pixels as apkrt_window_add or, at a turn, apkrt_window_set_size gave
/// it (never the drawable size). Returns 0, or -1 for a window that is removed or unknown.
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

/// The windows' buffers for drawing by the CPU (ADR-0043, decision 3), graphics/host's. The runtime calls them on the
/// guest's thread: lock from window_lock, once the registry found the window, with the guest's struct
/// (guest_interface.h); post from window_post, also for a window removed meanwhile. Each answers 0 or a negative Linux
/// errno. removed comes from apkrt_window_remove on the app's thread, once the registry let the window go, so that
/// its buffers can go too.
/// - A lock may still come for an ID right after its removed, since the registry's check and the call are apart: the
///   buffers answer -ENODEV and keep nothing of it. A call already past the registry when apkrt_set_window_buffers
///   changes or clears them still reaches the old functions and context.
/// - removed and post may run at once for the same ID: a guest thread in unlockAndPost passes the present gate as
///   "gone" while the app's thread is inside removed. The buffers synchronize the two themselves.
/// - lock and post must not wait long: they run inside a private syscall that guest signals cannot interrupt, and the
///   present gate before the post is the only wait. A post without a locked buffer answers -EINVAL.
struct apkrt_window_buffers {
    int (*lock)(void *context, uint64_t id, struct apkrt_window_buffer *buffer);
    int (*post)(void *context, uint64_t id);
    void (*removed)(void *context, uint64_t id);
    void *context;
};

/// For graphics/host: sets the windows' buffers, which the runtime copies, or takes them away with NULL. Returns 0,
/// or -1 when a function is NULL.
int apkrt_set_window_buffers(const struct apkrt_window_buffers *buffers);

// MARK: - Hardware buffers (ADR-0058 a))

/// The largest hardware buffer, 1 GiB, a 16384-pixel square of four bytes a pixel: the runtime refuses a description
/// whose sizes and bytes per pixel make more, and the host's buffers one whose rows, as they align them, make more.
#define APKRT_HARDWARE_BUFFER_MAX_BYTES (UINT64_C(1) << 30)

/// What the host's buffers make: AHardwareBuffer_Desc's fields (guest_interface.h's struct apkrt_hardware_buffer).
struct apkrt_hardware_buffer_description {
    uint32_t width, height, layers, format;
    uint64_t usage;
};

/// For graphics/host: the memory of the guest's AHardwareBuffers (ADR-0058 a)). The runtime has checked a description
/// (sizes not 0, a format of the NDK's or the VNDK's, at most APKRT_HARDWARE_BUFFER_MAX_BYTES) and keeps the IDs,
/// references and locks; the buffers see a handle of theirs, never 0. Each function runs on the calling guest thread
/// under no lock of the runtime's, and waits for nothing but the GPU's work on the buffer:
/// - supported: 1 for a description they can make, else 0;
/// - allocate: 0 with the handle and the stride in pixels; -EINVAL for a description they do not take, -ENOMEM;
/// - lock: 0 with the address of the first pixel, valid until the matching unlock, and the bytes per pixel and per
///   row, for usage's CPU read and write bits; a lock may come while the buffer is locked, each with its unlock.
///   libnativewindow keeps the rect, and what a lock of several layers gives is between it and the buffers;
/// - unlock: 0;
/// - free: at the last release, after the unlocks of every lock still open.
/// An answer other than 0 outside -4095..-1 reaches the guest as -EIO. A buffer stays with the functions that made
/// it: setting others or NULL changes only what comes next, so the context must live as long as the process.
struct apkrt_hardware_buffers {
    int (*supported)(void *context, const struct apkrt_hardware_buffer_description *description);
    int (*allocate)(void *context, const struct apkrt_hardware_buffer_description *description, uint64_t *handle,
                    uint32_t *stride);
    int (*lock)(void *context, uint64_t handle, uint64_t usage, void **address, uint32_t *bytes_per_pixel,
                uint32_t *bytes_per_row);
    int (*unlock)(void *context, uint64_t handle);
    void (*free)(void *context, uint64_t handle);
    void *context;
};

/// For graphics/host: sets the hardware buffers' functions, which the runtime copies, or takes them away with NULL.
/// Returns 0, or -1 when a function is NULL.
int apkrt_set_hardware_buffers(const struct apkrt_hardware_buffers *buffers);

/// For graphics/host (ADR-0058 b)): the guest's hardware buffer `id`, AHardwareBuffer_getId's, as the host's buffers
/// made it: 0 with its handle and, where not NULL, the context of the table that made it, its description and its
/// stride; -EINVAL for an ID that is not there, or a NULL `handle`. Takes a reference of the host's own, which keeps
/// the buffer beyond the guest's last release until apkrt_hardware_buffer_release_handle of the same ID; no release
/// of the guest's drops it. Any thread; it runs none of the table's functions.
int apkrt_hardware_buffer_acquire_handle(uint64_t id, uint64_t *handle, void **context,
                                         struct apkrt_hardware_buffer_description *description, uint32_t *stride);

/// Drops one reference that apkrt_hardware_buffer_acquire_handle took for `id`; the last of all frees the buffer
/// through its own table, after the unlocks of the locks still open. An ID without one is ignored.
void apkrt_hardware_buffer_release_handle(uint64_t id);

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

/// For graphics/host: the display's refresh grid, for presentation times on it (ADR-0041 decision 11): the last
/// tick's time and the period as that tick left it, both of the same tick. A NULL pointer is skipped. Returns 0,
/// or -1 before the first tick, filling nothing.
int apkrt_vsync_grid(int64_t *time_ns, int64_t *period_ns);

/// A new input channel, whose ID the host sends with APKRT_EVENT_INPUT_QUEUE_CREATED. Returns the ID, from 1,
/// never reused; 0 without memory. The events posted before the guest's queue connects wait in it.
uint64_t apkrt_input_add(void);

/// Posts a motion event (struct apkrt_motion_event) to the input channel `id`, after those before it.
/// `pointer_ids` holds `pointer_count` IDs, 1 to APKRT_MOTION_POINTERS of them, rising, each 0 to 31;
/// `sample_times` holds `sample_count` times, the history's oldest first and the event's own last; `coords`
/// holds sample_count × pointer_count places, by sample, then pointer index. As Android's reader makes them,
/// `action` is a MOVE, the only one with history; DOWN or UP of one pointer; CANCEL; or POINTER_DOWN or
/// POINTER_UP of two pointers or more, with the index of its pointer. A MOVE keeps its newest
/// APKRT_MOTION_SAMPLES samples.
/// A full channel (256 events) keeps the guest's gestures whole, as Android's dispatcher does. It merges the
/// oldest pair of neighbouring MOVEs with the same pointers and down time for a MOVE, which counts as the last
/// event (the earlier's samples become the later's history, keeping the newest APKRT_MOTION_SAMPLES), and
/// drops the MOVE when no pair can merge. It takes UP, POINTER_UP and CANCEL, up to 17 events beyond 256. It
/// refuses a DOWN, and ends the gesture at a POINTER_DOWN with a CANCEL of the pointers the guest has; the rest
/// of such a gesture is refused, its UP or CANCEL included, also once the channel has room again.
/// Returns 1 when the channel took the event, else 0: an unknown ID, a channel whose queue has closed, an
/// event that is not sound, one the channel refuses, or no memory.
int apkrt_input_post(uint64_t id, int32_t action, int64_t down_time, uint32_t pointer_count,
                     const int32_t *pointer_ids, uint32_t sample_count, const int64_t *sample_times,
                     const struct apkrt_pointer_coords *coords);

// MARK: - Audio output (ADR-0036, decisions 4 to 6)
//
// The guest's libaaudio opens output streams through the runtime (guest_interface.h), which keeps each stream's ring
// of interleaved float32 frames and its state, and asks the host for a node that plays it. A stream's handle is the
// runtime's: at least 64 (its slot's generation above the slot's index), so a host may keep smaller IDs for its own
// streams, and never valid again once the stream is closed. Start, pause and stop never reach the host:
// its node plays from the open to the close, and a stream that does not play reads nothing.

/// The hardware a stream opens on, as the host's audio session reports it.
struct apkrt_audio_hardware {
    double sample_rate;         // Hz
    double io_buffer_duration;  // seconds; the stream's burst is this long, in frames at the stream's rate to the
                                // nearest (halves up), at least 1
};

/// The host's side of the guest's audio output (apkrt_set_audio_host).
struct apkrt_audio_host {
    void *context;  // passed to both functions
    /// Opens `stream`: `channels` interleaved float32 channels at `sample_rate` Hz, 0 for the hardware's rate;
    /// `performance_mode` is AAudio's, NONE 10, POWER_SAVING 11 or LOW_LATENCY 12. Fills `hardware` and returns 0,
    /// or returns -1 when it cannot. From then on until close returns, the host's render block reads the stream with
    /// apkrt_audio_read.
    int (*open)(void *context, uint64_t stream, double sample_rate, int32_t channels, int32_t performance_mode,
                struct apkrt_audio_hardware *hardware);
    /// The stream is closed: once for each open that returned 0, disconnected streams included.
    void (*close)(void *context, uint64_t stream);
};

/// For the app: the host's audio side, for the streams opened from now on; NULL removes it, and the guest's opens
/// fail then. Both functions run on the guest thread that opens or closes a stream, inside its syscall, where the
/// runtime holds no lock: they may block, and may call apkrt_audio_disconnected and apkrt_audio_set_output. Returns
/// 0, or -1 when `host` lacks open or close.
int apkrt_set_audio_host(const struct apkrt_audio_host *host);

/// For the render block, on the real-time thread: copies up to `frames` frames of the stream's ring, interleaved
/// float32, into `buffer` and returns how many; `host_time` is the first one's AudioTimeStamp.mHostTime, for the
/// guest's timestamps. A stream that does not play (not started, paused, stopped and played out, disconnected, closed
/// or unknown) gives 0. A started one whose guest wrote too little gives what there is, and counts an xrun once the
/// stream has played since its start. It never blocks, takes no lock, allocates nothing and runs no guest code; a
/// call for a closed stream is harmless. One thread at a time reads a stream: its node's render block.
int64_t apkrt_audio_read(uint64_t stream, float *buffer, int64_t frames, uint64_t host_time);

/// For the app: a route change changed the hardware's rate or burst, so the stream is DISCONNECTED, and the guest's
/// error callback follows. The host drops its node itself; the close still comes.
void apkrt_audio_disconnected(uint64_t stream);

/// For the app, after each start of its engine and after each route change that disconnects no stream: the output's
/// AudioUnit, whose audio workgroup (kAudioOutputUnitProperty_OSWorkgroup) the guest's stream threads join, and the
/// output's latency in nanoseconds, from a render to the speaker, which the guest's timestamps add. NULL, with the
/// latency as it is, when the engine stops or an interruption begins. The workgroup may be stale by then (iOS stops
/// the engine in the background without a word): a thread whose join fails goes on without it. The runtime reads the
/// property with the AudioToolbox the unit comes from, so the app need not link AudioToolbox for it.
void apkrt_audio_set_output(void *audio_unit, int64_t output_latency);

/// The stream's xruns so far, as AAudioStream_getXRunCount gives them, for the host's log, also inside the host's
/// close; 0 for a closed or unknown stream.
int32_t apkrt_audio_xruns(uint64_t stream);

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

/// Sets the OpenGL ES version that ro.opengles.version gives (ADR-0044 d)), as major << 16 | minor: 0x30001 while
/// ANGLE draws on its Vulkan backend, 0x30000, the default, on Metal. graphics/host's minato_graphics_set_angle_backend
/// calls it, before the program starts, as the providers are set. Returns 0, or -1 for another version.
int apkrt_set_opengles_version(uint32_t version);

/// Whether the app holds android.permission.INTERNET (ADR-0054 step c2a), from its manifest. Without it, AF_INET and
/// AF_INET6 sockets answer EPERM, as netd's BPF program makes Android's kernel answer, and the guest's DNS proxy answers
/// only from the hosts file, failing every other name and address as Android's proxy fails a uid whose networking is
/// blocked, without the host's resolver. Granted until set. Only before the program starts: 0, or -1
/// once a start was tried, from which on the process keeps what it has.
int apkrt_set_internet(int granted);

#ifdef __cplusplus
}
#endif

#endif
