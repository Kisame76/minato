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
    APKRT_EVENT_NATIVE_WINDOW_RESIZED,    // as NATIVE_WINDOW_CREATED: right after it with the same size, as Android 15
                                          // calls onNativeWindowResized after onNativeWindowCreated, and at a turn
                                          // with the size apkrt_window_set_size gave the same window before the
                                          // post (ADR-0038 c), which ANativeWindow_getWidth/getHeight answer
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
    APKRT_REQUEST_WINDOW_FORMAT,   // guest only: a0 the window's format, a PixelFormat value in its low 32 bits
                                   // (ANativeActivity_setWindowFormat)
    APKRT_REQUEST_WINDOW_OPAQUE,   // host only: the argument is 1 or 0, reported when the window's opacity changes
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
/// AWINDOW_FLAG_KEEP_SCREEN_ON (0x80) changes, in the order of the changes. It keeps the window's format too,
/// from RGB_565 on, which NativeActivity sets at its start, and reports APKRT_REQUEST_WINDOW_OPAQUE when the
/// window's opacity changes, in the order of the changes: a window is opaque unless its format has alpha, by
/// Android 15's PixelFormat.formatHasAlpha. So the session starts opaque, without a report. It keeps each
/// window's frame rate as well and reports APKRT_REQUEST_FRAME_RATE when it changes, only for a window the host
/// has not removed.
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
/// window_size, libnativewindow's (ADR-0038 c): x0 the window's ID, x1 the address of two int32_t that it fills
/// with the window's width and height in pixels as of now, one size whole also during a turn
/// (apkrt_window_set_size). Returns 0; -EINVAL for a window that is removed or that the host never gave, -EFAULT
/// for address 0. The number follows the audio's.
#define APKRT_NR_WINDOW_SIZE 4118
/// session_configuration, for any guest code (ADR-0039, config-test): x0 a struct apkrt_configuration, which the
/// runtime fills with the session's, as session_connect does, but without the event pipe, from any thread and any
/// number of times. It is the configuration of apkrt_session_begin: a turn does not change it (ADR-0038). Returns 0;
/// -ENOENT without a session, -EFAULT for NULL.
#define APKRT_NR_SESSION_CONFIGURATION 4119
/// window_lock, libnativewindow's ANativeWindow_lock (ADR-0043, decision 3): x0 the window's ID, x1 a struct
/// apkrt_window_buffer, in which the guest asks for a buffer of its width, height and format, the rectangle in dirty
/// being what it will draw (the whole buffer when the app gives none). The host's buffers (apkrt_set_window_buffers)
/// answer on the calling thread with a buffer that the guest writes until window_post: its stride and bits, and in
/// dirty what the guest must draw, the whole buffer when they could not copy the last posted pixels into it. Returns
/// 0; -ENOSYS without the host's buffers, on a host that draws nothing by the CPU; -EFAULT for address 0; -EINVAL for
/// ID 0 or an ID the host never gave; -ENODEV once the window is removed, as Android's abandoned BufferQueue answers;
/// else the buffers' answer, -EINVAL for a format they do not take.
#define APKRT_NR_WINDOW_LOCK 4120
/// window_post, libnativewindow's ANativeWindow_unlockAndPost (ADR-0043, decision 3), after the window's present gate:
/// x0 the window's ID. The host's buffers present the buffer of window_lock on the window's layer and take it back;
/// for a window removed meanwhile, they only take it back and answer -ENODEV. Returns 0; -ENOSYS without the host's
/// buffers, -EINVAL for ID 0 or an ID the host never gave; else the buffers' answer, -EINVAL without a locked buffer.
#define APKRT_NR_WINDOW_POST 4121

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
/// vsync ID of the tick's number (from 1) + k and its expected present k periods after timeline 0's. Timeline 0
/// presents at target_time + period, 2 periods after the frame time at every rate: graphics/host sends a present
/// that near without a presentation time, and such a present reaches the iPhone's screen 2 refreshes after its
/// drawable (ADR-0041 decision 11). Each deadline is one period before its present, a guess (ADR-0036) settled by
/// the lead: a deadline at the frame time would have passed already, so timeline 0's is target_time.
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

// MARK: - Audio output (ADR-0036, decisions 5 and 6)
//
// libaaudio's output streams. The runtime keeps each stream's ring of interleaved float32 frames and its state in
// host memory, where the host's render block reads them (apkrt_audio_read in apkrt.h); the guest opens, writes,
// controls and waits through the syscalls below. A stream's handle is the runtime's: never 0, and never valid again
// once the stream is closed. Times are in nanoseconds on the guest's CLOCK_MONOTONIC; a deadline is such a time.

/// The ring holds this many bursts (the capacity); the guest fills this many at first (the buffer size).
#define APKRT_AUDIO_BURSTS 8
#define APKRT_AUDIO_FIRST_BURSTS 2
/// A stream's channels: 1 to this.
#define APKRT_AUDIO_CHANNELS 8

/// A stream's state, with AAudio's numbers (aaudio_stream_state_t). The only transient one is STARTING, which the
/// host's first read after the start ends.
enum apkrt_audio_state {
    APKRT_AUDIO_OPEN = 2,
    APKRT_AUDIO_STARTING = 3,
    APKRT_AUDIO_STARTED = 4,
    APKRT_AUDIO_PAUSED = 6,
    APKRT_AUDIO_FLUSHED = 8,
    APKRT_AUDIO_STOPPED = 10,
    APKRT_AUDIO_DISCONNECTED = 13,
};

/// audio_control's commands.
enum apkrt_audio_command {
    APKRT_AUDIO_START = 1,
    APKRT_AUDIO_PAUSE,
    APKRT_AUDIO_FLUSH,
    APKRT_AUDIO_STOP,
    APKRT_AUDIO_BUFFER_SIZE,  // the argument: the frames the guest may fill
};

/// What audio_open takes and gives.
struct apkrt_audio_config {
    int32_t sample_rate;       // in: Hz, 0 for the hardware's; out: the stream's
    int32_t channels;          // in: 1 to APKRT_AUDIO_CHANNELS
    int32_t performance_mode;  // in: AAudio's, NONE 10, POWER_SAVING 11 or LOW_LATENCY 12, for the host's IO buffer
    int32_t frames_per_burst;  // out: the host's IO buffer in frames at the stream's rate, to the nearest (halves up),
                               // at least 1
    int32_t capacity;          // out: APKRT_AUDIO_BURSTS bursts
    int32_t buffer_size;       // out: APKRT_AUDIO_FIRST_BURSTS bursts
    int32_t hardware_rate;     // out: Hz
    int32_t reserved;
};

/// What audio_status gives.
struct apkrt_audio_status {
    int32_t state;           // enum apkrt_audio_state
    uint32_t sequence;       // changes with what a guest may wait for: at every read or flush that takes frames, every
                             // change of the state or the buffer size, and at the close
    int64_t frames_written;  // by the guest since the open
    int64_t frames_read;     // by the host since the open; flushed frames count as read
    int32_t buffer_size;
    int32_t xruns;           // reads of a started stream that found fewer frames than asked, once it has played
    int64_t position;        // the first frame of the host's newest read that took frames; -1 before any
    int64_t time;            // when that frame is heard: the read's host time plus the output's latency
};

/// audio_open, libaaudio's: x0 a struct apkrt_audio_config. Opens an output stream, and the host adds a node that
/// plays it. Returns the stream's handle, above 0; -EFAULT for NULL; -EINVAL for a rate other than 0 or 8000 to
/// 1600000, channels outside 1 to APKRT_AUDIO_CHANNELS or a performance mode AAudio lacks; -ENODEV when the host has
/// no audio (apkrt_set_audio_host); -EMFILE when 64 streams are open; -ENOMEM without memory; -EIO when the host
/// cannot open the stream.
#define APKRT_NR_AUDIO_OPEN 4111
/// audio_close: x0 the handle. Ends the stream, whose waiters answer -EBADF, and the host drops its node. Returns 0;
/// -EBADF for a handle that is no open stream.
#define APKRT_NR_AUDIO_CLOSE 4112
/// audio_control: x0 the handle, x1 an enum apkrt_audio_command, x2 its argument. Returns 0, for BUFFER_SIZE the size
/// set: the argument, at least a burst and at most the capacity. The states follow AAudio's requests:
///   - START from OPEN, PAUSED, FLUSHED or STOPPED gives STARTING, which the host's next read makes STARTED;
///   - PAUSE gives PAUSED from every state, once no read of the host is under way;
///   - FLUSH from OPEN, PAUSED, STOPPED or FLUSHED gives FLUSHED, once no read is under way, and drops the frames not
///     read yet;
///   - STOP gives STOPPED at once: the frames a started stream holds at the STOP still play, as AudioTrack's stop
///     lets them, and those of a stream that did not play stay, as do frames written after the STOP, until the next
///     START;
///   - each answers 0 at once in the state it gives, and -EPERM in any other.
/// -EPIPE once the stream is DISCONNECTED, -EBADF for a handle that is no open stream, -EINVAL for another command.
#define APKRT_NR_AUDIO_CONTROL 4113
/// audio_write: x0 the handle, x1 x2 frames of interleaved float32 (x2 × the channels), x3 a deadline. Takes as many
/// frames as the buffer size leaves room for, and waits until the deadline for room for the rest; a deadline that
/// has passed, 0 included, does not wait. Returns the frames taken; when none were taken, -EPIPE once the stream is
/// DISCONNECTED and -EINTR for a deliverable signal; -EFAULT for NULL with frames, -EINVAL for x2 below 0, -EBADF for
/// a handle that is no open stream, also when the stream closes during the wait.
#define APKRT_NR_AUDIO_WRITE 4114
/// audio_status: x0 the handle, x1 a struct apkrt_audio_status, which it fills. Returns 0; -EFAULT for NULL, -EBADF
/// for a handle that is no open stream.
#define APKRT_NR_AUDIO_STATUS 4115
/// audio_wait: x0 the handle, x1 a sequence of audio_status, x2 a deadline. Returns 0 once the stream's sequence is
/// another, at once when it is already; -ETIMEDOUT once the deadline has passed, -EINTR for a deliverable signal,
/// -EBADF for a handle that is no open stream, also when the stream closes during the wait.
#define APKRT_NR_AUDIO_WAIT 4116
/// audio_join, a stream thread's: x0 the handle, x1 1 to join, 0 to leave. Joins the calling thread to the audio
/// workgroup of the host's output (apkrt_audio_set_output) with a time constraint of the stream's burst, first
/// leaving one it joined before; 0 at once when the thread is in the output's workgroup already. The thread also
/// leaves when it ends. Returns 0; -ENOENT when the output has no workgroup, -EBADF for a handle that is no open
/// stream, -EINVAL for an x1 other than 0 and 1, or os_workgroup_join's error as a negated Linux errno.
#define APKRT_NR_AUDIO_JOIN 4117

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

/// The buffer of window_lock (ADR-0043, decision 3): ANativeWindow_Buffer's fields and the dirty ARect.
struct apkrt_window_buffer {
    int32_t width;      // in: the size the guest asks for, which the buffer has
    int32_t height;
    int32_t stride;     // out: in pixels, from a row to the next
    int32_t format;     // in: an AHardwareBuffer format (WINDOW_FORMAT_*), which the buffer has
    void *bits;         // out: the first pixel's address
    int32_t dirty[4];   // left, top, right, bottom; in: what the guest will draw, out: what it must draw
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

struct AConfiguration;

/// In libandroid.so, version APKRT_PRIVATE: `session` filled with the session's configuration
/// (session_configuration), and `config` with the values decision 9 chooses from it, which
/// AConfiguration_fromAssetManager gives for NativeActivity's asset manager too. For libandroid_runtime, so that
/// Java's Configuration and DisplayMetrics come from the same source (ADR-0039, config-test). Returns 0; ENOENT
/// without a session, EFAULT when `config` or `session` is NULL.
int minato_AConfiguration_fromSession(struct AConfiguration *config, struct apkrt_configuration *session);

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

struct AAudioStreamStruct;

/// In libaaudio.so, unversioned as its other names, from the stream's data callback: the frames of this call are the
/// stream's as of now, so that only a flush or a stop after this call drops them (ADR-0036 c2). Without it, one after
/// the call began does: the stream's thread cannot see when the callback takes its frames. libOpenSLES claims them
/// under the player's lock, which also orders its flushes and stops. From another thread it does nothing.
void minato_AAudioStream_claimCallbackFrames(struct AAudioStreamStruct *stream);

#ifdef __cplusplus
}
#endif

#endif
