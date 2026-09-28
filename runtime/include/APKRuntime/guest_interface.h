// Interface between the runtime (host) and our parts of the guest libc (ADR-0021). Linker64 does
// two things in-process that libc.so relies on: it sets up the main thread before any constructor
// runs, and it serves libdl through the __loader_* functions of ld-android.so. Here the runtime's
// loader does the loading, and two pieces of guest code, built from bionic's sources with its own
// structures, do the rest:
//   - __apkrt_start_main_thread, added to libc.so (runtime/bionic/src/libc_start.cpp);
//   - ld-android.so (runtime/bionic/src/ld_android.cpp), which reaches the loader through
//     apkrt_loader, a table the runtime fills in before guest code runs.
// Plain C, shared by the runtime and the guest build, so the layout is the same on both sides.
#ifndef APKRUNTIME_GUEST_INTERFACE_H
#define APKRUNTIME_GUEST_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One ELF object as the runtime loaded it: the program or a library.
struct apkrt_object {
    uint64_t bias;      // host address of ELF address 0
    const void *phdr;   // its program headers, in memory
    uint64_t phnum;
    const char *name;   // DT_NEEDED name, or the program's path
    // Written by __apkrt_start_main_thread for an object with a PT_TLS segment:
    uint64_t tls_module_id;  // 0 when the object has no TLS
    int64_t tls_offset;      // of its static TLS block from the thread pointer
};

/// What __apkrt_start_main_thread needs.
struct apkrt_start {
    void *start_stack;              // argc, argv, envp and auxv, as the program sees them
    struct apkrt_object *objects;   // the program first, then its libraries in load order
    size_t object_count;
};

/// In libc.so, version APKRT_PRIVATE. Called on the program thread after relocation and before any
/// constructor, where linker64 would do the same: the temporary and then the final thread pointer,
/// the main thread's pthread_internal_t and bionic_tls, the stack guard, and the static TLS of every
/// object (the program's first). Returns 0, or -1 when there are too many objects with TLS.
typedef int (*apkrt_start_main_thread_function)(struct apkrt_start *start);
#define APKRT_START_MAIN_THREAD "__apkrt_start_main_thread"

/// In libc.so, version APKRT_PRIVATE: linker64's TLS descriptor resolvers, which libc.so carries
/// here (bionic's linker/arch/arm64/tlsdesc_resolver.S). The loader points every
/// R_AARCH64_TLSDESC descriptor at one of them, as linker64 does.
enum apkrt_tlsdesc_resolver {
    APKRT_TLSDESC_STATIC,           // argument: the variable's offset from the thread pointer
    APKRT_TLSDESC_DYNAMIC,          // argument: a TlsDynamicResolverArg (dlopen'ed libraries)
    APKRT_TLSDESC_UNRESOLVED_WEAK,  // argument: the addend; the address comes out as that value
    APKRT_TLSDESC_RESOLVER_COUNT
};
#define APKRT_TLSDESC_RESOLVERS "__apkrt_tlsdesc_resolvers"

/// In libc.so, version APKRT_PRIVATE: registers the TLS segment of a library dlopen loaded as a
/// dynamic TLS module, as linker64's register_soinfo_tls does. Writes the module ID into the
/// object and the TLS generation it starts with to `generation`; returns the module ID, 0 when the
/// object has no TLS segment or no module is free.
typedef uint64_t (*apkrt_register_tls_module_function)(struct apkrt_object *object, uint64_t *generation);
#define APKRT_REGISTER_TLS_MODULE "__apkrt_register_tls_module"

/// In libc.so, version APKRT_PRIVATE: the restorer of signal handlers without SA_RESTORER, in place of
/// the vDSO's rt_sigreturn trampoline, which Linux gives them.
#define APKRT_SIGRETURN "__apkrt_sigreturn"

/// In ld-android.so, version APKRT_PRIVATE: what the runtime offers libdl. Every function may be
/// null; ld-android.so then reports that it cannot do what was asked. The runtime publishes new
/// objects by raising object_count (a release store) after the entry is written; readers load it
/// with acquire. dlopen writes a Linux errno value to `error_number` when its search failed to open a
/// file somewhere, which linker64 leaves in errno; ld-android.so sets errno to it. ld-android.so calls
/// all but dlerror under a lock of its own, one thread at a time (ADR-0025, decision 8).
struct apkrt_loader {
    int target_sdk_version;
    const struct apkrt_object *objects;  // for dl_iterate_phdr
    size_t object_count;
    void *(*dlopen)(const char *name, int flags, const void *caller, int *error_number);
    void *(*dlsym)(void *handle, const char *symbol, const char *version, const void *caller);
    int (*dlclose)(void *handle);
    const char *(*dlerror)(void);
    int (*dladdr)(const void *address, void *info);  // info: a Dl_info
};
#define APKRT_LOADER "apkrt_loader"

/// The private syscall "bind host table" (ADR-0030, decision 3; ADR-0028, decision 11), which a front
/// lib's first constructor makes once, through the veneer like every syscall: x0 the lib's name
/// (tools/frontgen's: "vulkan", "gles", "egl"), x1 its table, x2 the table's length in slots, slot 0
/// included, x3 the hash of the generator's name list. It returns 0 once the provider graphics/host set
/// for the name (apkrt_set_host_provider) has filled slots 1 to length - 1 and the runtime has put its
/// entry apkrt_host_call into slot 0; -ENOENT (-2) without a provider; -EINVAL (-22) when the provider
/// refuses, the length is 0, the name or the table is NULL or the name is longer than 63 bytes. Any
/// other invalid pointer is a fault of the runtime, as with the loader's syscalls.
///
/// A stub reaches the entry with the host function in x16 and the arguments where Apple's ABI puts
/// them, the stack pointer as it is, by a branch to slot 0. The entry calls the host function as host
/// code, and guest signals wait for its return (ADR-0030, decision 1).
#define APKRT_NR_BIND_HOST_TABLE 4101

// MARK: - The session (ADR-0028, decisions 2 to 4 and 7)
//
// The host posts events (apkrt_session_post in apkrt.h), activity-host reads them from the event pipe,
// calls the NativeActivity callback of each and answers it; libandroid and activity-host report what the
// guest asks of the host; the front libs pass the window's gate. These layouts are the same on both
// sides, and apkrt.h includes this header for the app.

/// The events, one per NativeActivity callback, with their arguments (struct apkrt_event).
enum apkrt_event_type {
    APKRT_EVENT_CREATE = 1,
    APKRT_EVENT_START,
    APKRT_EVENT_RESUME,
    APKRT_EVENT_PAUSE,
    APKRT_EVENT_STOP,
    APKRT_EVENT_DESTROY,
    APKRT_EVENT_SAVE_INSTANCE_STATE,
    APKRT_EVENT_WINDOW_FOCUS_CHANGED,     // a0: 1 when the window has the focus, else 0
    APKRT_EVENT_NATIVE_WINDOW_CREATED,    // a0: the window's ID (apkrt_window_add); a1, a2: width, height in pixels
    APKRT_EVENT_NATIVE_WINDOW_RESIZED,    // as NATIVE_WINDOW_CREATED; in M3 only right after it, with the same size,
                                          // as Android 15 calls onNativeWindowResized after onNativeWindowCreated.
                                          // A window's size is fixed: activity-host keeps the one it was created
                                          // with, and a new size comes as a new window
    APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED,  // a0: the window's ID
    APKRT_EVENT_NATIVE_WINDOW_DESTROYED,  // a0: the window's ID
    APKRT_EVENT_INPUT_QUEUE_CREATED,      // a0: the input channel's ID (ADR-0036), 0 for a queue without events
    APKRT_EVENT_INPUT_QUEUE_DESTROYED,    // a0: as INPUT_QUEUE_CREATED
    APKRT_EVENT_CONTENT_RECT_CHANGED,     // a0: left | top << 32; a1: right | bottom << 32 (32 bits each)
    APKRT_EVENT_CONFIGURATION_CHANGED,
    APKRT_EVENT_LOW_MEMORY,
};

/// One record of the event pipe, written whole or not at all. Serial numbers start at 1 and follow the
/// pipe's order.
struct apkrt_event {
    uint32_t type;  // enum apkrt_event_type
    uint32_t serial;
    uint64_t arguments[3];
};

/// The app as the host describes it at the session's begin (struct apkrt_session in apkrt.h), for
/// AConfiguration (ADR-0028, decision 9).
struct apkrt_configuration {
    double scale;          // the screen's native scale (the layer's contentsScale), e.g. 3
    int32_t width;         // the screen's size in pixels, in the session's orientation
    int32_t height;
    int32_t orientation;   // ACONFIGURATION_ORIENTATION_PORT (1) or ACONFIGURATION_ORIENTATION_LAND (2)
    int32_t night;         // ACONFIGURATION_UI_MODE_NIGHT_NO (1) or ACONFIGURATION_UI_MODE_NIGHT_YES (2)
    int32_t refresh_rate;  // the display's highest rate in frames per second, e.g. 120
    char language[4];      // ISO 639, e.g. "de"; NUL-terminated
    char country[4];       // ISO 3166, e.g. "DE"; empty when there is none
};

/// What the guest asks of the host (ADR-0028, decision 4).
enum apkrt_request_type {
    APKRT_REQUEST_FINISH = 1,      // ANativeActivity_finish; the host then sends the quit sequence
    APKRT_REQUEST_WINDOW_FLAGS,    // guest only: a0 the flags to add, a1 those to remove (setWindowFlags)
    APKRT_REQUEST_KEEP_SCREEN_ON,  // host only: the argument is 1 or 0, reported when the flag changes
    APKRT_REQUEST_START_FAILED,    // a0: activity-host's text (dlerror's) about why the app cannot start,
                                   // NUL-terminated; the host gets at most 1023 bytes of it
    APKRT_REQUEST_FRAME_RATE,      // ANativeWindow_setFrameRate (ADR-0036, decision 2): a0 the window's ID, a1 the rate
                                   // in frames per second as a float's bits, 0 for no preference; the host gets the
                                   // bits when the rate of a window it has not removed changes
};

/// The kinds of the window's gate.
enum apkrt_gate_kind {
    APKRT_GATE_PRESENT = 1,      // at every present
    APKRT_GATE_BACKGROUND = 2,   // at every call that commits GPU work
    APKRT_GATE_PRESENT_TRY = 3,  // the present's gate without a wait, for a swap that flushes before it waits
};

/// The private syscalls of ADR-0028 (decision 11) for activity-host, libandroid and the front libs,
/// through the veneer like every syscall.
///
/// session_connect, activity-host's, once: x0 a struct apkrt_configuration, which the runtime fills with
/// the session's. Returns the event pipe's read end as a new guest descriptor (close-on-exec), whose
/// records are struct apkrt_event; -ENOENT without a session, -EFAULT for NULL, -EMFILE when the guest's
/// table is full, which closes the pipe's end for good: every connect after one that got that far answers
/// -EBUSY.
#define APKRT_NR_SESSION_CONNECT 4102
/// session_answer, activity-host's: x0 the serial number of the event whose callback returned. Returns 0;
/// -ENOENT without a session, -EINVAL for 0 or a serial not posted yet.
#define APKRT_NR_SESSION_ANSWER 4103
/// session_request: x0 an enum apkrt_request_type, x1 and x2 its arguments. Returns 0; -ENOENT without a
/// session, -EFAULT for APKRT_REQUEST_START_FAILED without a text, -EINVAL for another type. The runtime
/// keeps the window flags and reports only APKRT_REQUEST_KEEP_SCREEN_ON to the host, when
/// AWINDOW_FLAG_KEEP_SCREEN_ON (0x80) changes, in the order of the changes. It keeps each window's frame rate
/// as well and reports APKRT_REQUEST_FRAME_RATE when it changes, only for a window the host has not removed.
#define APKRT_NR_SESSION_REQUEST 4104
/// window_gate, the front libs': x0 the window's ID, x1 an enum apkrt_gate_kind. Returns 0 to go on and
/// 1 when the window is gone, or waits:
///   - present: 0 while the window's gate is open, a wait while it is closed, 1 once the window is gone
///     or for an ID the host never gave; -EINVAL for ID 0;
///   - background: 1 for a window that is gone or unknown, else 0 in the foreground and a wait in the
///     background; ID 0 for calls without a window, such as a queue submit, which only wait;
///   - present_try: never waits: 0 while the window's gate is open, 2 while it is closed, 1 once the window
///     is gone or for an ID the host never gave; -EINVAL for ID 0. libEGL's swap flushes when it sees 2 and
///     then waits with present (ADR-0029, decision 15).
/// Every change the host makes ends a wait's round. A deliverable signal ends the wait with -EINTR once
/// its handler ran; with SA_RESTART the runtime makes the call again. -EINVAL for another kind.
#define APKRT_NR_WINDOW_GATE 4105

// MARK: - The vsync and input channels (ADR-0036, decision 1)
//
// The host posts the ticks of its frame clock and its motion events (apkrt_vsync_post and apkrt_input_post in
// apkrt.h), which the runtime keeps in host memory. Each consumer, a choreographer or an input queue, holds a
// guest descriptor of its channel for its looper, readable while the channel holds something unread, and
// takes the records with the syscalls below. Times are in nanoseconds on the guest's CLOCK_MONOTONIC.

/// The frame timelines of a tick (AChoreographerFrameCallbackData).
#define APKRT_FRAME_TIMELINES 3

/// One frame timeline.
struct apkrt_frame_timeline {
    int64_t vsync_id;
    int64_t expected_present_time;
    int64_t deadline;
};

/// What vsync_read gives: the newest tick the channel asked for, and the display's period. Timeline k has the
/// vsync ID of the tick's number (from 1) + k, its expected present at target_time + (k + 1) × period and its
/// deadline one period earlier, so the preferred one's deadline is target_time. A guess (ADR-0036, step a
/// measures it against the drawables' presentedTime), settled by the lead: a deadline at the frame time would
/// have passed already.
struct apkrt_vsync {
    int64_t time;                 // the tick's vsync, Choreographer's frame time; 0 before the first tick
    int64_t target_time;          // when the next frame is due
    int64_t period;               // the display's frame period: a tick whose target_time - time differs by more
                                  // than 1 % from it makes that the period; before the first tick 10^9 / the
                                  // session's refresh_rate, 0 without a session
    uint32_t preferred_timeline;  // 0
    uint32_t reserved;
    struct apkrt_frame_timeline timelines[APKRT_FRAME_TIMELINES];
};

/// Android's MAX_POINTERS: the pointers of a motion event.
#define APKRT_MOTION_POINTERS 16
/// The samples of a motion event: its history and, last, its own.
#define APKRT_MOTION_SAMPLES 16

/// A pointer's place in one sample.
struct apkrt_pointer_coords {
    float x;            // in the window's pixels; raw x and y are the same, as the window fills the screen
    float y;
    float pressure;     // 0 to 1
    float touch_major;  // in pixels
};

/// A motion event of the touchscreen as input_read gives it (AMotionEvent). Its source, tool type and device
/// are libandroid's in M3: AINPUT_SOURCE_TOUCHSCREEN, AMOTION_EVENT_TOOL_TYPE_FINGER and one device.
struct apkrt_motion_event {
    int32_t action;          // AMOTION_EVENT_ACTION_*: DOWN 0, UP 1, MOVE 2, CANCEL 3, and POINTER_DOWN 5 and
                             // POINTER_UP 6 with the pointer's index << 8
    uint32_t pointer_count;  // 1 to APKRT_MOTION_POINTERS
    uint32_t sample_count;   // 1 to APKRT_MOTION_SAMPLES: the history, oldest first, then the event's own sample
    uint32_t reserved;
    int64_t down_time;       // the time of the gesture's DOWN
    int32_t pointer_ids[APKRT_MOTION_POINTERS];  // in the order of the pointers: ascending, 0 to 31
    int64_t sample_times[APKRT_MOTION_SAMPLES];  // the last is the event's time
    struct apkrt_pointer_coords coords[APKRT_MOTION_SAMPLES][APKRT_MOTION_POINTERS];  // by sample, then pointer index
};

/// vsync_connect, AChoreographer's, once per instance: a new vsync channel. Returns its guest descriptor
/// (close-on-exec), readable while the channel holds a tick it asked for or a period it has not read; -EMFILE
/// when the guest's table is full, -ENOMEM without memory, or the host's error when it has no descriptors left
/// for the channel's pipe. Closing the descriptor ends the channel.
#define APKRT_NR_VSYNC_CONNECT 4106
/// vsync_request: x0 the channel's descriptor. The next tick goes to the channel (requestNextVsync); asking
/// again before it comes changes nothing. Returns 0; -EBADF for a descriptor that is no vsync channel.
#define APKRT_NR_VSYNC_REQUEST 4107
/// vsync_read: x0 the channel's descriptor, x1 a struct apkrt_vsync, which it fills; the descriptor is then no
/// longer readable. Returns 1 when a tick came since the last read, else 0; -EBADF for a descriptor that is no
/// vsync channel, -EFAULT for NULL.
#define APKRT_NR_VSYNC_READ 4108
/// input_connect, activity-host's at APKRT_EVENT_INPUT_QUEUE_CREATED (minato_AInputQueue_create): x0 the input
/// channel's ID. Returns a guest descriptor (close-on-exec), readable while the channel holds an event, as the
/// wake byte of InputQueue.cpp; -ENOENT for an ID the host never gave, 0 included; -EBUSY for a channel that
/// has a descriptor, had one or is being connected. After -EMFILE when the guest's table is full, -ENOMEM
/// without memory, or the host's error when it has no descriptors left for the channel's pipe, a connect can
/// be made again. Closing its last descriptor ends the channel: the host's later posts fail.
#define APKRT_NR_INPUT_CONNECT 4109
/// input_read: x0 the channel's descriptor, x1 a struct apkrt_motion_event. Takes the oldest event into it;
/// once the channel is empty, the descriptor is no longer readable (pop and drain). Returns 0; -EAGAIN when
/// there is none, -EBADF for a descriptor that is no input channel, -EFAULT for NULL.
#define APKRT_NR_INPUT_READ 4110

/// What a front lib needs of an ANativeWindow (ADR-0028, decision 7); graphics/host turns the ID into the
/// window's layer.
struct minato_window_info {
    uint64_t id;            // the host window's ID
    int32_t width;          // the window's size in pixels
    int32_t height;
    int32_t buffer_width;   // those of ANativeWindow_setBuffersGeometry, 0 while none is set
    int32_t buffer_height;
    int32_t buffer_format;
};

struct ANativeWindow;

/// In libnativewindow.so, version APKRT_PRIVATE: fills `info` and returns 0, or returns EINVAL for
/// anything that is not a window.
int minato_ANativeWindow_getInfo(struct ANativeWindow *window, struct minato_window_info *info);

struct AAssetManager;
struct AInputQueue;

/// In libandroid.so, version APKRT_PRIVATE: the asset manager of the APK at the guest path `apk` (ADR-0028,
/// decision 10), for ANativeActivity. It reads the ZIP's central directory once and keeps a descriptor of the
/// APK; AConfiguration_fromAssetManager gives the values decision 9 chooses from `configuration`, the
/// session's. NULL when the APK cannot be read or its central directory is not sound, or without memory.
struct AAssetManager *minato_AAssetManager_create(const char *apk, const struct apkrt_configuration *configuration);

/// In libandroid.so, version APKRT_PRIVATE: a new input queue of the input channel `channel` (ADR-0036), for
/// onInputQueueCreated, connected to it (input_connect); for channel 0 or one it cannot connect to, a queue
/// without events. NULL without memory.
struct AInputQueue *minato_AInputQueue_create(uint64_t channel);

/// In libandroid.so, version APKRT_PRIVATE: frees a queue once onInputQueueDestroyed has returned, which
/// ends its channel.
void minato_AInputQueue_delete(struct AInputQueue *queue);

/// In libnativewindow.so, version APKRT_PRIVATE: a new window of the host window `id`, `width` × `height`
/// pixels in the format WINDOW_FORMAT_RGBX_8888, which NativeActivity's window has in Android 15, with one
/// reference for the caller (activity-host, at NATIVE_WINDOW_CREATED). NULL without memory.
struct ANativeWindow *minato_ANativeWindow_create(uint64_t id, int32_t width, int32_t height);

#ifdef __cplusplus
}
#endif

#endif
