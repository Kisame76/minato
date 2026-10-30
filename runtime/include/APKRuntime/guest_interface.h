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
    /// dlopen in a namespace, for android_dlopen_ext (ADR-0033 e0): `search_path`, the namespace's folders
    /// separated by colons, serves the library, the libraries loaded for it and their dlopen by name, ahead of
    /// where a plain dlopen looks, as linker64 searches the namespace a library belongs to.
    void *(*dlopen_in)(const char *name, int flags, const void *caller, int *error_number, const char *search_path);
    /// The loader's namespaces, as linker64 keeps them (ADR-0040 decision 4, ADR-0033 e2a): libdl_android's
    /// android_create_namespace, android_link_namespaces(_all_libs) (`sonames` null for all, else separated by
    /// colons), android_get_exported_namespace, and android_dlopen_ext in a namespace. A namespace is an opaque
    /// handle of the runtime's. Null on an older runtime, where ld-android.so keeps its own minimal namespaces.
    void *(*create_namespace)(const char *name, const char *ld_library_path, const char *default_library_path,
                              uint64_t type, const char *permitted_path, void *parent, const void *caller);
    int (*link_namespaces)(void *from, void *to, const char *sonames);
    void *(*exported_namespace)(const char *name);
    void *(*dlopen_namespace)(const char *name, int flags, const void *caller, int *error_number, void *space);
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
    // A SurfaceView's surface (ADR-0043 c4), which the guest asks for with APKRT_REQUEST_SURFACE:
    APKRT_EVENT_SURFACE_CREATED,          // the answer to a make: a0 the surface's token; a1 its window's ID
                                          // (apkrt_window_add); a2 width | height << 32 in pixels (32 bits each)
    APKRT_EVENT_SURFACE_RESIZED,          // as SURFACE_CREATED: the answer to a place with another size, once the
                                          // window has it (apkrt_window_set_size); a place that only moves gets none
    APKRT_EVENT_SURFACE_DESTROYED,        // a0: the token; a1: the window's ID. The host took the surface away on its
                                          // own, at the background or the end; the guest still removes the token
    // A window of the guest's own (ADR-0041, gap 4): a dialog's, a popup's, a toast's, a translucent activity's or a
    // sub-window's, which the guest asks for with APKRT_REQUEST_WINDOW:
    APKRT_EVENT_WINDOW_CREATED,           // the answer to a make: a0 the window's token; a1 its window's ID
                                          // (apkrt_window_add); a2 width | height << 32 in pixels (32 bits each)
    APKRT_EVENT_WINDOW_RESIZED,           // as WINDOW_CREATED: the answer to a place with another size
    APKRT_EVENT_WINDOW_DESTROYED,         // a0: the token; a1: the window's ID. The host took the window away on its
                                          // own, at the background or the end, a sub-window's before its parent's;
                                          // the guest still removes the token
    APKRT_EVENT_PICK_RESULT,              // the answer to APKRT_REQUEST_PICK (ADR-0057 step a): a0 the request's ID;
                                          // a1 the result code (Activity.RESULT_CANCELED 0, RESULT_OK -1, as a 32-bit
                                          // int in the low bits); a2 the number of picks, whose details the host wrote
                                          // to the manifest before it posted this. The picks of a document request
                                          // (kinds 1 and 2, ADR-0057 step b): the host clones each file into the
                                          // folder the guest sees as /storage/emulated/0/Download (CLONE NAMES:
                                          // see below), and writes the manifest, the file ".minato/picks/<ID in
                                          // decimal>" below the app's data folder (ApplicationInfo.dataDir): UTF-8,
                                          // the clones' names, one per line, each line ended by "\n"; a name is 1 to
                                          // 255 bytes, without "/", NUL, control, separator or formatting characters,
                                          // not "." or "..", each once, and as many lines as a2 (1 to 1000). The host
                                          // writes it to a file of another name and renames it, before it posts this,
                                          // and never reads it back; the guest reads it, deletes it, and takes the
                                          // answer as a cancel when it is not so. HOW THE HOST WRITES: the app can
                                          // write its data folder and Download too, and leave links, planted files and
                                          // loops there. The host takes the data folder from its own records, opens it
                                          // and Download once, then works only from directory descriptors: folders
                                          // below them with openat(O_DIRECTORY|O_NOFOLLOW) and mkdirat, files created
                                          // with O_CREAT|O_EXCL|O_NOFOLLOW (or a failing fclonefileat); a name is busy
                                          // only when that fails with EEXIST, never by stat or exists; the manifest
                                          // goes to "<ID>.partial" (on EEXIST that name is unlinked with unlinkat and
                                          // made again, or named at random) and is renamed with renameat on the same
                                          // descriptor; an existing file is never opened for writing; a link where a
                                          // folder should be makes the answer a cancel. apk-run's tools/pick_host.h is
                                          // the reference, tests/pick_host_test.cpp plays the attacks. The clone's
                                          // name is made plain by the host: the user's file name with every byte that
                                          // is no UTF-8 and every control, separator, bidirectional, zero-width or
                                          // other formatting character and "/" replaced by "_", cut to 255 bytes
                                          // between code points keeping the extension, "" "." ".." as "Untitled";
                                          // a numbered "<name> (<n>).<ext>" when the name is busy.
                                          // The picks of a photo request (kind 5, step c): the clones go to the
                                          // folder ".minato/photos" below the data folder (as many as the request's
                                          // maximum at most), and each manifest line is the clone's name, then the
                                          // width, height, duration in milliseconds, time taken in milliseconds
                                          // since 1970 and orientation (0, 90, 180 or 270), tab-separated, decimal
                                          // without leading zeros; the name's extension (jpg, jpeg, png, gif, webp,
                                          // bmp, mp4, webm, 3gp) says the MIME type. The host makes JPEG and MP4 of
                                          // what the user picked, as iOS has no HEIF decoder in the guest;
                                          // CLONE NAMES (kinds 1, 2 and 5 alike): the host makes every clone as a
                                          // new file with O_EXCL, so a clone is never written over a file that was
                                          // there, and takes the first name of the plain one and its "<name> (<n>)"
                                          // forms that is free at that moment. The one exception: a file the user
                                          // picks again gets the clone of the earlier pick back, as long as that is
                                          // still a regular file of that name in the same folder (the same document
                                          // as on Android); a clone the app removed or replaced by a folder or link
                                          // is made anew, and so is one the app has not written to when the user's file
                                          // has changed since (Android's document is live; a clone with the app's
                                          // edits stays, the write-back decides). A host that remembers a clone by
                                          // the file it came from forgets every other file's entry for that name when
                                          // a file takes it, since a freed name goes to whichever file asks first.
                                          // A name is therefore new unless it is the same document.
                                          // WRITE-BACK (kinds 1 and 2, ADR-0057's addendum of 2026-10-23): what the
                                          // app writes into a clone reaches the user's file. The host keeps a record
                                          // per clone in ITS OWN data (the original's security-scoped bookmark, the
                                          // original's and the clone's size, time and SHA-256 at the clone), and one
                                          // second after the last close of a descriptor the app wrote through, at the
                                          // session's background and at its end, compares the clone with the record:
                                          // unchanged, nothing is done; changed, and the original still has the
                                          // recorded content (read under NSFileCoordinator), the clone replaces it
                                          // (temp file and rename); else nothing is written, the app's version goes to
                                          // the host's own folder and the user decides. The clone is read with
                                          // O_NOFOLLOW from the Download descriptor, a link or folder there is a
                                          // conflict, and the guest's cleaning of unkept clones is harmless because the
                                          // host holds the app's version. A record never outlives its clone: the
                                          // host forgets it (and stops watching the file) when the user lets a
                                          // conflict go and when the clone is missing at two checks in a row (the
                                          // 100 ms scans of an untouched clone, the write-backs of a changed one) or
                                          // at the session's final one; a save that unlinks the clone and renames
                                          // another file to its name between two checks is a conflict, not a loss.
                                          // A file the app makes under that name later is its own. Each record
                                          // keeps the clone's identity (device, inode and birth time as made),
                                          // across restarts too, and a file of the clone's
                                          // name with another identity is not the clone: it is never written back,
                                          // its version is kept and the user decides (so a save that renames a file
                                          // over the clone is a conflict, not a silent write). A clone with no
                                          // record is not reused by a pick again (it may hold a version the user
                                          // rejected), and a name that a record with the app's work holds (DIRTY or
                                          // CONFLICT) is not given to a new clone even when the guest deleted that
                                          // clone, so its kept copy is never orphaned. A clone is copied only when
                                          // no writer can hold it: apkrt_watch_writers of its watch is 0 (a copy is
                                          // never made while it is above 0, also at the background) and its size and
                                          // time have stood still for the second, since a descriptor opened again
                                          // may be writing in bursts. A clone that has no watch (count -1) is copied
                                          // after the close the watch reported, or at the session's end, when no
                                          // writer is left. A write that fails is tried
                                          // again after 1, 2, 4, 8 and 16 s; the sixth failure is a conflict with the
                                          // app's version kept. tools/pick_host.h's WriteBack is the reference and
                                          // tests/pick_host_test.cpp plays the cases.
                                          // The picks of a save or a folder request (kinds 3 and 4, step d): a2 is 1
                                          // and the manifest is ONE line, a path relative to /storage/emulated/0 with
                                          // plain names between single "/", no "." or ".." component, at most 1024
                                          // bytes and 16 components, no leading "/". Kind 3: the host made the file
                                          // there itself (O_EXCL through the descriptor of the folder the user
                                          // chose, never over an existing file), its name the request's title made
                                          // plain by the rule above, numbered when busy, so that the title is
                                          // FAT-valid (Android's buildValidFatFilename). Kind 4: the folder the user
                                          // chose, which the host has mounted into the guest (apkrt_mount_live) at
                                          // that path. The host remembers what the user chose (security-scoped
                                          // bookmarks) in ITS OWN data and never in anything the guest can write:
                                          // not the app's data folder, not uri-grants
    APKRT_EVENT_OPEN_RESULT,              // the answer to APKRT_REQUEST_OPEN (ADR-0062): a0 the request's ID; a1 the
                                          // result code (Activity.RESULT_CANCELED 0, RESULT_OK -1, as a 32-bit int in
                                          // the low bits): RESULT_OK once the browser sheet, the composer, the share
                                          // sheet or iOS's open reported that it was shown or done, RESULT_CANCELED
                                          // when the user cancelled or the host did not open it; a2 why, for the log:
                                          // 0 shown, 1 refused (scheme, URL or share file), 2 the session was in the
                                          // background, 3 no handler on this device. The share files (kind 4): the
                                          // app can write its data folder and leave links there, so the host opens
                                          // the data folder from its own records and then each of ".minato", "share"
                                          // and "<ID>" with openat(O_DIRECTORY|O_NOFOLLOW) on the one before, each
                                          // file with openat(O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC) and fstat for
                                          // a regular file (a link, a folder, a FIFO or a file over the host's size
                                          // limit refuses the share), and copies them (fclonefileat or a capped
                                          // copy) into a folder of its own for the sheet, where two names the file
                                          // system takes for one (Unicode normalization, case) refuse the share too;
                                          // it removes its copies when the sheet ends and never writes into the
                                          // share folder, which the guest removes after the answer. The guest uses
                                          // each ID once and ignores an answer to an ID it no longer waits for
    APKRT_EVENT_PERMISSION_RESULT,        // the answer to APKRT_REQUEST_PERMISSION (ADR-0063/0064), posted by
                                          // apkrt_permission_answer: a0 the request's ID, or 0 for an unsolicited
                                          // update (the host's foreground re-read of the iOS authorisation); a1 the
                                          // group (APKRT_PERMISSION_*); a2 the outcome 0 denied, 1 granted (fine for
                                          // location), 2 granted coarse (location only). checkSelfPermission answers
                                          // the stored outcome, DENIED before the first
    APKRT_EVENT_LOCATION,                 // a fix the location hub announces (ADR-0063): a0 the fix's sequence (for
                                          // APKRT_LOCATION_GET); a1 flags (APKRT_LOCATION_EVENT_* below): bit 0 a
                                          // one-shot answer to an APKRT_LOCATION_CURRENT, bit 1 the fix is coarse
                                          // (reduced accuracy), bit 2 a one-shot that resolved with no location (no
                                          // GET follows); a2 the one-shot's token when bit 0 is set, else 0. A
                                          // continuous fix (bit 0 clear) is fetched with GET by its sequence, or read
                                          // whole with APKRT_LOCATION_LAST (getLastKnownLocation)
    APKRT_EVENT_BATTERY,                  // the battery state changed (App's gap-1 proposal, 2026-10-29): a0 the
                                          // charge level in percent 0..100, or -1 over the full 64-bit field when
                                          // unknown; a1 the state (APKRT_BATTERY_STATE_* below), UIDevice.batteryState
                                          // passed through raw; a2 0. Posted only on a change while a session runs, as
                                          // Android's sticky ACTION_BATTERY_CHANGED broadcasts only on a change. A
                                          // newly-registered receiver reads the current value synchronously with
                                          // APKRT_NR_BATTERY (GET) before any event. Needs no permission, as the
                                          // Android broadcast does not
    APKRT_EVENT_IME_INSETS,               // the soft keyboard's height (ADR-0068 step 2d): a0 the part of the session's
                                          // view that the keyboard covers, from its top edge to the view's bottom, in
                                          // the session's pixels (0 when it is hidden); a1, a2 0. The host posts it
                                          // whenever the keyboard's frame changes (UIKit's keyboard notifications,
                                          // apk-run's model), also while it moves (the last value counts); the guest
                                          // makes the ime() inset of every window from the last one
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
    APKRT_REQUEST_SURFACE,         // guest only: a0 the address of a struct apkrt_surface_request (ADR-0043 c4)
    APKRT_REQUEST_WINDOW,          // guest only: a0 the address of a struct apkrt_window_request (ADR-0041, gap 4)
    APKRT_REQUEST_PICK,            // guest only (ADR-0057 step a): a0 the address of NUL-terminated text, a1 the
                                   // request's ID (not 0, not one that waits); the host gets the ID as `argument` and
                                   // the text as `text`: "<kind>\t<flags>\t<max>\t<MIME types>\t<title>". The runtime
                                   // refuses (-EINVAL) any text but this: a kind 1 to 5; flags 0 or 1, 1 only for
                                   // kinds 1, 2 and 5; a maximum of 0 to 1000 in decimal without leading zeros, other
                                   // than 0 only for kind 5; up to 8 MIME types of at most 100 bytes each ("*" only
                                   // as a whole type or subtype, as image/* or */*), separated by newlines, none is
                                   // allowed, and none for kind 4; a title of at most 200 bytes
                                   // of UTF-8 without control, separator or formatting characters, only for kind 3;
                                   // at most 1535 bytes in all. At most 4 requests wait at once (-EBUSY), and an ID
                                   // that waits is refused (-EEXIST). The PICK and EXPORT_MEDIA requests reach the
                                   // host's handler one at a time; requests of other types may reach it meanwhile. The
                                   // host answers once with APKRT_EVENT_PICK_RESULT, which ends the request
    APKRT_REQUEST_PICK_CANCEL,     // guest only: a0 the ID of a request that waits and the guest no longer wants (its
                                   // activity is gone); the host gets the ID as `argument` and no text, and gives no
                                   // answer. -EINVAL for ID 0, -ENOENT for an ID that does not wait
    APKRT_REQUEST_OPEN,            // guest only (ADR-0062): a0 the address of NUL-terminated text, a1 the request's ID
                                   // (not 0, not one that waits); the host gets the ID as `argument` and the text as
                                   // `text`: "<kind>\t<field>\t<field>...", each field percent-encoded UTF-8 (RFC
                                   // 3986's unreserved bytes raw and never as %XX, every other byte as %XX), an empty
                                   // field being "". Kinds: 1 VIEW, one field, an absolute URL of scheme http, https,
                                   // tel or maps; 2 MAIL, one field, a mailto: URL (RFC 6068: to in the path, cc, bcc,
                                   // subject and body in the query); 3 SMS, one field, an sms: URL; 4 SHARE, 3 + 2n
                                   // fields: the subject, the text, n in decimal without leading zeros (0 to 10), n
                                   // names of the files the guest put into ".minato/share/<ID in decimal>" below the
                                   // app's data folder before the request, then n MIME types, one per name. The runtime
                                   // refuses (-EINVAL) any text but this: a kind 1 to 4 and its number of fields; every
                                   // % followed by two hex digits; each field valid UTF-8 once decoded, without NUL; a
                                   // URL field of 1 to 4096 bytes decoded, without control characters, spaces (any of
                                   // Unicode's Zs), line or paragraph separators or format characters (Cf), whose
                                   // scheme (up to the first ":", ASCII case ignored) is the kind's; a subject of at
                                   // most 1000 bytes and a text of at most 65536 bytes decoded, any code point but NUL;
                                   // a name of 1 to 255 bytes decoded, without "/" and the URL's characters but U+0020,
                                   // not "." or "..", each once byte by byte; a MIME type of 3 to 100 bytes
                                   // "<type>/<subtype>" of RFC 6838's characters ("*" only as a whole type or subtype,
                                   // as image/* or */*); at most 204800 bytes in all. At most 4 requests wait at once
                                   // (-EBUSY), and an ID that waits is refused (-EEXIST). The OPEN requests reach the
                                   // host's handler one at a time; requests of other types may reach it meanwhile. The
                                   // host answers once with APKRT_EVENT_OPEN_RESULT, which ends the request. The host
                                   // checks again and is the last word: it opens only http, https, mailto, tel, sms and
                                   // maps, and nothing while the session is in the background
    APKRT_REQUEST_OPEN_CANCEL,     // guest only: a0 the ID of an open request that waits and the guest no longer wants
                                   // (its activity is gone); the host gets the ID as `argument` and no text, drops what
                                   // it shows for it and gives no answer. -EINVAL for ID 0, -ENOENT for an ID that does
                                   // not wait
    APKRT_REQUEST_EXPORT_MEDIA,    // guest only (ADR-0057 step e): a0 the address of NUL-terminated text, a1 0 (else
                                   // -EINVAL); a finished image or video the app made in MediaStore. The host gets
                                   // argument 0 and the text "<kind>\t<path>": kind 1 an image, 2 a video; the path
                                   // of the file below the app's data folder (ApplicationInfo.dataDir), "/"-separated
                                   // names, at most 16 of them and 1024 bytes, no leading "/", each 1 to 255 bytes of
                                   // UTF-8, not "." or "..", without "/", NUL, C0 and C1 controls, DEL, line and
                                   // paragraph separators, zero-width and bidirectional controls (U+200B to U+200F,
                                   // U+2028 to U+202E, U+2060 to U+206F), U+FEFF and U+FFF9 to U+FFFB. Anything else
                                   // is allowed, spaces and the other format characters included: it is a path the
                                   // host opens, not a name it shows (the host may show the last name, with such
                                   // characters taken out). The text ends at its first NUL. The runtime
                                   // refuses (-EINVAL) any other text, -EFAULT like a pick for text it cannot read.
                                   // There is no answer and no limit of waiting requests. The handler runs at once
                                   // on the guest's thread, under the lock of the pick requests, so it must be
                                   // quick: it only TAKES the file, by walking the path from its own record of the
                                   // data folder with openat and O_NOFOLLOW at every component, O_DIRECTORY for the
                                   // folders and O_NONBLOCK for the file (a FIFO the app made must not hold it),
                                   // keeps the descriptor of a regular file with one link (st_nlink 1) within
                                   // the limits it sets (apk-run's model: 1 GiB a file, 1000 files and 4 GiB in
                                   // all for a session), and returns. The copy is made afterwards, off that thread,
                                   // from that descriptor: PhotoKit takes a URL or data, so the host copies the
                                   // descriptor (no more than the size it saw) into a file in a folder of its own and
                                   // gives Photos only that copy, never the guest's path, which the app could swap
                                   // for a link between the check and the use. A copy that fails leaves nothing. The
                                   // host adds the copy to Photos when iOS allows (add-only access after its one
                                   // prompt) and otherwise does nothing. The kind is advisory: the host does not
                                   // trust the file to be a picture
    APKRT_REQUEST_PERMISSION,      // guest only (ADR-0063/0064): a0 the address of NUL text "<group>\t<detail>", a1 the
                                   // request's ID (not 0, not one that waits). The host gets the ID as `argument` and
                                   // the text as `text`: group 1 location, 2 microphone (APKRT_PERMISSION_* below);
                                   // detail 1 fine wanted, 2 coarse only (location), 0 (microphone). The runtime
                                   // refuses -EINVAL any other text, an unknown group or bad detail, an ID of 0;
                                   // -EEXIST an ID that waits, -EBUSY when 4 wait. One at a time to the host handler,
                                   // as PICK and OPEN. The host shows its pre-prompt sheet then iOS's prompt and
                                   // answers once with apkrt_permission_answer, which records the grant and posts
                                   // APKRT_EVENT_PERMISSION_RESULT, ending the request
    APKRT_REQUEST_PERMISSION_CANCEL,  // guest only: a0 the ID of a permission request that waits and the guest no
                                   // longer wants; the host gets the ID and drops its sheet, no answer. -EINVAL for ID
                                   // 0, -ENOENT for an ID that does not wait
    APKRT_REQUEST_IME,             // guest only (ADR-0068, part 1): a0 the address of NUL-terminated text, a1 the
                                   // connection's serial. The host gets the serial as `argument` and the text:
                                   // "start\t<inputType>\t<imeOptions>\t<capsMode>\t<selStart>\t<selEnd>\t<fieldId>
                                   // \t<hint>" (the numbers decimal, the hint last, cut by the guest to fit) when a
                                   // view that takes text is served with a new connection (a1 not 0, a fresh one each
                                   // time), "show" and "stop" (a1 the live serial, not 0: the keyboard is wanted, the
                                   // view is no longer served) and "hide" (a1 the live serial or 0: the keyboard is
                                   // not wanted). The host answers with text records for that serial
                                   // (APKRT_TEXT_*), which the guest drops once the serial is not the live one. No
                                   // answer ends the request. The runtime refuses -EINVAL any other text or a serial
                                   // of 0 where one is needed, a text of 512 bytes or more, -EFAULT an unreadable one
};

// MARK: - Permissions (ADR-0063 location, ADR-0064 microphone)
//
// A dangerous permission group the guest asks for with APKRT_REQUEST_PERMISSION; the host answers with
// apkrt_permission_answer (apkrt.h), which records the grant in the runtime and posts APKRT_EVENT_PERMISSION_RESULT.
#define APKRT_PERMISSION_LOCATION   1
#define APKRT_PERMISSION_MICROPHONE 2
// APKRT_REQUEST_PERMISSION's detail (the text's second field); for location, the precision the app asked.
#define APKRT_PERMISSION_DETAIL_NONE   0  // microphone, and any group without a precision
#define APKRT_PERMISSION_DETAIL_FINE   1  // location: ACCESS_FINE_LOCATION wanted
#define APKRT_PERMISSION_DETAIL_COARSE 2  // location: only ACCESS_COARSE_LOCATION
// APKRT_EVENT_PERMISSION_RESULT's outcome (a2), and the runtime's stored grant.
#define APKRT_PERMISSION_DENIED         0
#define APKRT_PERMISSION_GRANTED        1  // location: fine; microphone: granted
#define APKRT_PERMISSION_GRANTED_COARSE 2  // location only

/// What a SurfaceView asks of its surface (ADR-0043 c4), APKRT_REQUEST_SURFACE's.
enum apkrt_surface_op {
    APKRT_SURFACE_MAKE = 1,  // a surface for a token that has none
    APKRT_SURFACE_PLACE,     // the surface's new frame, z order or buffer size; its format stays the make's
    APKRT_SURFACE_REMOVE,    // the guest is done with the surface, after surfaceDestroyed; frees the token
};

/// A surface request, 48 bytes. A format change is a remove and a make, as Android 15's SurfaceView makes a new
/// surface for one. For a remove only the op and the token count.
struct apkrt_surface_request {
    uint32_t op;       // enum apkrt_surface_op
    int32_t z;         // SurfaceView's sublayer: -2 media (its default), -1 media overlay, 1 on top, 2 a sub-panel's;
                       // below the window's layer when negative, above it when positive
    uint64_t token;    // the guest's name of the surface, not 0, one per live surface of the session
    int32_t left;      // the frame in the window's pixels, not empty
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t width;     // the buffer's size in pixels; both 0 for the frame's size (setSizeFromLayout)
    int32_t height;
    int32_t format;    // a PixelFormat value, RGB_565 (4) by default
    int32_t opaque;    // the runtime's, for the host: 1 unless the format has alpha (PixelFormat.formatHasAlpha)
};

/// What a window of the guest's own asks of the host (ADR-0041, gap 4), APKRT_REQUEST_WINDOW's.
enum apkrt_window_op {
    APKRT_WINDOW_MAKE = 1,  // a window for a token that has none
    APKRT_WINDOW_PLACE,     // the window's new frame, buffer size, alpha or dim; type, parent, group and format stay
                            // the make's
    APKRT_WINDOW_REMOVE,    // the guest is done with the window; frees the token
};

/// A window request, 72 bytes without padding. A later change takes a new request type, never a new meaning for a
/// field. For a remove only the op and the token count.
struct apkrt_window_request {
    uint32_t op;        // enum apkrt_window_op
    int32_t type;       // LayoutParams.type: 1 to 99 an application window, 1000 to 1999 a sub-window, 2005 a toast
    uint64_t token;     // the guest's name of the window, not 0, one per live window of the session
    uint64_t parent;    // a sub-window's window: a live requested one's token, or 0 for the session's window; 0 for
                        // every other type
    int32_t left;       // the frame in the session's pixels, not empty
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t width;      // the buffer's size in pixels; both 0 for the frame's size
    int32_t height;
    int32_t format;     // a PixelFormat value
    int32_t opaque;     // the runtime's, for the host: 1 unless the format has alpha (PixelFormat.formatHasAlpha)
    uint32_t group;     // an application window's activity as a sort key: 0 on the session's window, larger above;
                        // 0 for sub-windows and toasts
    uint32_t alpha;     // LayoutParams.alpha, a float's bits, 0 to 1
    uint32_t dim;       // dimAmount with FLAG_DIM_BEHIND, else 0, a float's bits, 0 to 1
    uint32_t reserved;  // 0
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
/// APKRT_REQUEST_SURFACE (ADR-0043 c4) answers -EOPNOTSUPP without the host's surface handler; -EFAULT when the
/// request cannot be read whole; -EINVAL for an unknown op, token 0, and for a make or place: an empty frame or one
/// with a side above 2^20, z not -2, -1, 1 or 2, only one of width and height 0, or a buffer side, the frame's for 0
/// and 0, outside 1 to 8192. The runtime keeps the live tokens, at most 16: a make answers -EEXIST for a live token and
/// -ENOSPC beyond 16, a place or remove -ENOENT for a token that is not live, and the remove frees it. A token stays
/// live until its remove, also after SURFACE_DESTROYED. The host gets a copy with the buffer's size and the opacity.
/// APKRT_REQUEST_WINDOW (ADR-0041, gap 4) answers as APKRT_REQUEST_SURFACE for the reading, the op, the token, the
/// frame and the size, and has no z. A make or place also answers -EINVAL for alpha or dim that is not finite or not
/// within 0 to 1, and for reserved not 0. A make answers -EINVAL for a type outside 1 to 99, 1000 to 1999 and 2005,
/// for a parent on another type than a sub-window, and for a group on a sub-window or a toast. A sub-window's parent
/// is 0 for the session's window or a live window of these that is no sub-window itself: -ENOENT for one that is not
/// live, -EINVAL for a sub-window. The live tokens, at most 16, are apart from the surfaces': -EEXIST, -ENOSPC and
/// -ENOENT answer as there, and the remove of a window that still has live sub-windows answers -EBUSY. The host gets
/// a copy with the buffer's size and the opacity, and a place carries the make's type, parent, group and format.
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

// MARK: - Hardware buffers (ADR-0058 a))
//
// libnativewindow's AHardwareBuffer: the runtime keeps the process's buffers by an ID of its own, with their
// description, references and locks; the host's buffers (apkrt_set_hardware_buffers in apkrt.h) make, lock, unlock
// and free their memory, IOSurfaces in the process, which the guest reads and writes at a lock's address.

/// What hardware_buffer does, its x0.
enum apkrt_hardware_buffer_op {
    APKRT_HARDWARE_BUFFER_ALLOCATE = 1,  // x1 a struct apkrt_hardware_buffer: the description in, id and stride out
    APKRT_HARDWARE_BUFFER_SUPPORTED,     // x1 a struct apkrt_hardware_buffer: the description; answers 1 or 0
    APKRT_HARDWARE_BUFFER_DESCRIBE,      // x1 a struct apkrt_hardware_buffer: id in, the description and stride out
    APKRT_HARDWARE_BUFFER_ACQUIRE,       // x1 the ID: one reference more
    APKRT_HARDWARE_BUFFER_RELEASE,       // x1 the ID: one reference less; the last frees the buffer, unlocked first
    APKRT_HARDWARE_BUFFER_LOCK,          // x1 a struct apkrt_hardware_buffer: id and usage in, address,
                                         // bytes_per_pixel and bytes_per_stride out
    APKRT_HARDWARE_BUFFER_UNLOCK,        // x1 the ID: the last lock's end
};

/// AHardwareBuffer_Desc's fields with the runtime's ID, and what a lock gives (56 bytes). An operation leaves the
/// fields it does not name alone; reserved must be 0.
struct apkrt_hardware_buffer {
    uint64_t id;                             // the runtime's, never 0 and never reused: AHardwareBuffer_getId
    uint32_t width, height, layers, format;  // AHardwareBuffer_Desc's; format an AHARDWAREBUFFER_FORMAT_*
    uint64_t usage;                          // AHARDWAREBUFFER_USAGE_*; a lock's: the CPU's bits alone
    uint32_t stride;                         // pixels from a row's start to the next's (BLOB: its width)
    uint32_t bytes_per_pixel, bytes_per_stride;  // a lock's, for AHardwareBuffer_lockAndGetInfo
    uint32_t reserved;
    uint64_t address;                        // a lock's first pixel, for the guest's reads and writes until UNLOCK
};

/// hardware_buffer, libnativewindow's AHardwareBuffer_* (ADR-0058 a)): x0 an enum apkrt_hardware_buffer_op, x1 its
/// argument. Android's rules for a description are libnativewindow's; the runtime checks what keeps the host and its
/// registry sound: sizes not 0, a format of Android's (for its bytes per pixel), the size within 1 GiB with every
/// product checked, reserved 0, the IDs, and the locks. Returns 0, SUPPORTED 1 or 0; -ENOSYS for an ALLOCATE without
/// the host's buffers (SUPPORTED: 0); -EFAULT for address 0; -EINVAL for an unknown op, ID 0 or one the runtime does
/// not have, a size of 0, a format Android does not have, reserved not 0, a LOCK with other than the CPU's usage bits,
/// an UNLOCK without a lock; -ENOMEM for a buffer above 1 GiB [to check: Android 15's answer, ahb-test]; else the
/// buffers'. The number follows the loader's namespace calls (loader_syscalls.h, 4122 to 4126).
#define APKRT_NR_HARDWARE_BUFFER 4127

// MARK: - Video decoders (ADR-0060 e2b3)
//
// libmediandk's AMediaCodec and the framework's MediaCodec decode video through the host's decoders
// (apkrt_set_video_decoders in apkrt.h, VideoToolbox in graphics/host): the runtime keeps the process's decoders by
// IDs of its own, copies the guest's input to the host and the host's decoded frames to the guest, checking every size
// and address on the way. A decoder for a surface (ADR-0060 e2c) keeps its frames on the host instead: the guest gets
// a token for each and has the host draw it into one of its hardware buffers, a slot of the surface's queue.

/// What video_decoder does, its x0; x1 the address of a struct apkrt_video_decoder, or the ID for FLUSH and DESTROY.
enum apkrt_video_decoder_op {
    APKRT_VIDEO_DECODER_CREATE = 1,  // type, width, height and the codec data (data, size) in; id out
    APKRT_VIDEO_DECODER_QUEUE,       // id, an access unit (data, size), time_us and flags in
    APKRT_VIDEO_DECODER_DEQUEUE,     // id and a buffer (data, size: its capacity) in; the oldest decoded frame out
    APKRT_VIDEO_DECODER_FLUSH,       // the ID: every frame queued or decoded is dropped
    APKRT_VIDEO_DECODER_DESTROY,     // the ID: the decoder goes, after the calls on it still running
    APKRT_VIDEO_DECODER_RENDER,      // id, a frame's token (data) and a hardware buffer's ID (size): the frame drawn
    APKRT_VIDEO_DECODER_DISCARD,     // id and a frame's token (data): the frame goes undrawn
    APKRT_VIDEO_DECODER_RENDER_WINDOW,  // id, a frame's token (data), a host window's ID (size), the present time
                                        // (time_us, 0 for at once) and the scaling (flags): the frame drawn (e2c3)
};

/// A decoder's stream: the codec's elementary stream, in Annex B with start codes as MediaCodec takes it.
enum apkrt_video_decoder_type {
    APKRT_VIDEO_DECODER_AVC = 1,   // video/avc, H.264
    APKRT_VIDEO_DECODER_HEVC = 2,  // video/hevc, H.265
};

/// flags: QUEUE's last input (size may be 0), and DEQUEUE's answer after the last frame (size 0).
#define APKRT_VIDEO_DECODER_END_OF_STREAM 1u

/// CREATE's flag: a decoder for a surface (ADR-0060 e2c), whose frames stay the host's. Its DEQUEUE writes no bytes:
/// it gives a frame's sizes, crop, time and flags with its token in `data` (never 0), size, stride and slice height 0,
/// and the frame waits for the RENDER or DISCARD of its token; at most APKRT_VIDEO_DECODER_MAX_WAITING (apkrt.h) wait
/// at once, and DEQUEUE answers -EAGAIN past them. A flush that succeeds or a destroy ends every token.
#define APKRT_VIDEO_DECODER_SURFACE 2u

/// RENDER_WINDOW's flag: the frame fills the window, cropped (MediaCodec's
/// VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING); without it the frame is stretched to the window (SCALE_TO_FIT).
#define APKRT_VIDEO_DECODER_CROP 4u

/// One call's arguments and answers (64 bytes). An operation leaves the fields it does not name alone.
/// DEQUEUE writes a frame as NV12: `slice_height` rows of `stride` bytes of Y, then (slice_height + 1) / 2 rows of
/// `stride` bytes of interleaved Cb and Cr, `size` bytes in all; width and height are the coded size, the crop's
/// rectangle (inclusive) the picture's.
struct apkrt_video_decoder {
    uint64_t id;            // the runtime's, never 0 and never reused
    uint32_t type, flags;   // an enum apkrt_video_decoder_type (CREATE); APKRT_VIDEO_DECODER_* flags
    uint32_t width, height; // CREATE: the stream's size; DEQUEUE: the frame's coded size
    uint64_t data;          // the guest's bytes: CREATE's codec data, QUEUE's access unit, DEQUEUE's buffer
    uint64_t size;          // their size; DEQUEUE: the buffer's capacity in, the frame's bytes out
    int64_t time_us;        // QUEUE's time, DEQUEUE's frame's
    uint32_t stride, slice_height;                           // DEQUEUE: the frame's rows
    uint16_t crop_left, crop_top, crop_right, crop_bottom;   // DEQUEUE
};

/// video_decoder (ADR-0060 e2b3): x0 an enum apkrt_video_decoder_op, x1 its argument. Returns 0; -ENOSYS without the
/// host's decoders; -EFAULT for a struct or bytes the guest cannot read or write; -EINVAL for an unknown op or type,
/// a width or height of 0 or above APKRT_VIDEO_DECODER_MAX_DIMENSION, codec data above
/// APKRT_VIDEO_DECODER_MAX_CODEC_DATA or an access unit above APKRT_VIDEO_DECODER_MAX_UNIT (apkrt.h); -EBADF for an ID
/// that is not there, or no more;
/// -EBUSY for a CREATE past APKRT_VIDEO_DECODER_MAX_DECODERS; DEQUEUE -EAGAIN while no frame is decoded, and
/// -ENOSPC with the frame's size and rows out (the frame stays) for a buffer too small. RENDER draws the frame of
/// token `data` into the guest's hardware buffer of ID `size` (AHardwareBuffer_getId's), scaled to the buffer's size,
/// and DISCARD drops it; either ends the token, whatever its answer but -EBADF and -EFAULT; -EINVAL for a decoder not
/// made for a surface, a token that does not wait, or a buffer that is not there, has more than one layer, or has a
/// format the host does not draw into. RENDER_WINDOW draws it into the host window of ID `size` (libnativewindow's
/// window of a SurfaceView or of the activity), presented at `time_us` of the guest's CLOCK_MONOTONIC (0, or more
/// than a second ahead: at once), and ends the token as RENDER does; -EINVAL as RENDER, for a time_us below 0 or past
/// INT64_MAX / 1000, for flags other than APKRT_VIDEO_DECODER_CROP and for a window without a Metal layer, -ENODEV
/// for a window of ID 0, not there or removed (ADR-0060 e2c3). Else the host's answer. The number follows
/// hardware_buffer's.
#define APKRT_NR_VIDEO_DECODER 4128

// MARK: - Media players (ADR-0060 e2e1)
//
// The framework's MediaPlayer plays through the host's player (apkrt_set_media_players in apkrt.h, AVPlayer in
// graphics/host): the guest keeps MediaPlayer's states and answers (libmedia's mediaplayer.cpp, in
// android_media_MediaPlayer.cpp), the host loads, plays and seeks. The runtime keeps the process's players by IDs of
// its own, as the video decoders', and checks every size, address and descriptor before the host acts. What the host
// tells the guest comes as records on the player's event descriptor, the read end of a pipe the runtime writes and
// the guest reads on a thread of its own: the source prepared or failed, a seek done, the end, the video's size, a
// frame to draw, and a callback source's read. No record is dropped: a guest that reads never fills the pipe (the
// lifecycle records are few, the frames at most APKRT_MEDIA_PLAYER_MAX_WAITING), and the records of one that does not
// wait in order for the pipe. An http or https source needs the app's INTERNET permission, which the runtime holds
// (ADR-0054 step c2a); its network security config is the guest's to check, as Android's media server leaves it to
// the app's process (MediaHTTPConnection): unlike location's, iOS has no network permission of an app's that the
// runtime could hold the guest to beyond INTERNET.

/// What media_player does, its x0; x1 the address of a struct apkrt_media_player.
enum apkrt_media_player_op {
    APKRT_MEDIA_PLAYER_CREATE = 1,  // id and events (the event descriptor: close-on-exec, non-blocking) out
    APKRT_MEDIA_PLAYER_SOURCE,      // id, kind and its fields in, once, before PREPARE
    APKRT_MEDIA_PLAYER_PREPARE,     // id: the host loads the source; PREPARED or ERROR follows as a record
    APKRT_MEDIA_PLAYER_START,       // id
    APKRT_MEDIA_PLAYER_PAUSE,       // id
    APKRT_MEDIA_PLAYER_SEEK,        // id, time_us and the mode (value: MediaPlayer's SEEK_*); SEEK_DONE follows
    APKRT_MEDIA_PLAYER_STATUS,      // id: time_us (the position), duration_us (-1: none), width, height, flags out
    APKRT_MEDIA_PLAYER_SET,         // id, the key (kind: an enum apkrt_media_player_key) and its value in
    APKRT_MEDIA_PLAYER_TRACKS,      // id and a buffer (data, size: its capacity) in; the tracks out, size their bytes
    APKRT_MEDIA_PLAYER_DATA,        // id, the READ's token, its bytes (data, size) and flags in
    APKRT_MEDIA_PLAYER_RENDER,      // id, a FRAME's token and a hardware buffer's ID (size): the frame drawn
    APKRT_MEDIA_PLAYER_RENDER_WINDOW,  // id, a FRAME's token, a host window's ID (size), the present time (time_us,
                                       // 0 for at once) and the scaling (flags): the frame drawn
    APKRT_MEDIA_PLAYER_DISCARD,     // id and a FRAME's token: the frame goes undrawn
    APKRT_MEDIA_PLAYER_DESTROY,     // id: the player goes, after the calls on it still running; every waiting
                                    // FRAME's token and the READ end, the event descriptor's pipe closes
};

/// SOURCE's kind.
enum apkrt_media_player_source {
    APKRT_MEDIA_PLAYER_FD = 1,        // fd (the guest's, which the runtime duplicates), offset and length (-1: to
                                      // the file's end), within the file's size
    APKRT_MEDIA_PLAYER_URL = 2,       // an http or https URL (data, size: UTF-8 without a NUL), checked by the guest
    APKRT_MEDIA_PLAYER_CALLBACK = 3,  // a MediaDataSource of length bytes (-1: unknown), read by READ records
};

/// SET's key.
enum apkrt_media_player_key {
    APKRT_MEDIA_PLAYER_VOLUME = 1,   // value: the left and right volumes as two floats (low and high 32 bits), 0 to 1
    APKRT_MEDIA_PLAYER_LOOPING = 2,  // value: 0 or 1
    APKRT_MEDIA_PLAYER_SPEED = 3,    // value: the speed as a float (low 32 bits), 1/8 to 8
};

/// STATUS's flags.
#define APKRT_MEDIA_PLAYER_PLAYING 1u

/// DATA's flags: the source failed the read (MediaDataSource.readAt below 0), with no bytes.
#define APKRT_MEDIA_PLAYER_READ_FAILED 1u

/// TRACKS' records, one per track of the source in its order (64 bytes): MediaPlayer.TrackInfo's type (1 video,
/// 2 audio, 3 timed text, 4 subtitle, 5 metadata, 0 unknown), the language (ISO 639-2, "und" for none) and the MIME
/// type, each NUL-terminated within its array.
struct apkrt_media_player_track {
    uint32_t type;
    char language[4];
    char mime[56];
};

/// One call's arguments and answers (96 bytes). An operation leaves the fields it does not name alone.
struct apkrt_media_player {
    uint64_t id;                   // the runtime's, never 0 and never reused
    uint32_t kind, flags;          // SOURCE's enum apkrt_media_player_source, SET's key; STATUS's and RENDER_WINDOW's
    int32_t fd, events;            // SOURCE's descriptor; CREATE's event descriptor
    uint32_t width, height;        // STATUS's video size (0 without video)
    uint64_t data, size;           // the guest's bytes: a URL, TRACKS' buffer, DATA's bytes; RENDER's buffer ID
    int64_t offset, length;        // SOURCE's
    int64_t time_us, duration_us;  // SEEK's and RENDER_WINDOW's time; STATUS's position and duration
    uint64_t token;                // DATA's READ, RENDER's, RENDER_WINDOW's and DISCARD's FRAME
    uint64_t value;                // SET's value, SEEK's mode
};

/// What the event descriptor gives, one record at a time (32 bytes).
enum apkrt_media_player_event_type {
    APKRT_MEDIA_PLAYER_PREPARED = 1,    // the video's width and height (ext1, ext2; 0 without), duration (value, in
                                        // microseconds; -1: none)
    APKRT_MEDIA_PLAYER_ERROR = 2,       // MediaPlayer's what and extra (ext1, ext2): a PREPARE's failure, or later
    APKRT_MEDIA_PLAYER_SEEK_DONE = 3,   // value: the position reached, in microseconds
    APKRT_MEDIA_PLAYER_COMPLETED = 4,   // the end, without looping
    APKRT_MEDIA_PLAYER_VIDEO_SIZE = 5,  // the video's width and height (ext1, ext2), changed
    APKRT_MEDIA_PLAYER_FRAME = 6,       // a frame to draw: its token and time (value, the guest's CLOCK_MONOTONIC in
                                        // microseconds, when it shows); it waits for its RENDER, RENDER_WINDOW or
                                        // DISCARD, at most APKRT_MEDIA_PLAYER_MAX_WAITING at once
    APKRT_MEDIA_PLAYER_READ = 7,        // a CALLBACK source's read of ext1 bytes at offset value: its token, one at a
                                        // time; DATA answers it (size 0: the end of the source, below 0 never)
};

struct apkrt_media_player_event {
    uint32_t type;
    int32_t ext1, ext2;
    uint32_t reserved;  // 0
    int64_t value;
    uint64_t token;
};

/// media_player (ADR-0060 e2e1): x0 an enum apkrt_media_player_op, x1 its argument. Returns 0; -ENOSYS without the
/// host's players; -EFAULT for a struct or bytes the guest cannot read or write; -EINVAL for an unknown op, kind or
/// key, a SOURCE twice or after PREPARE, a PREPARE without a SOURCE or twice, an fd source's offset or length outside
/// its file, a URL of 0 bytes, above APKRT_MEDIA_PLAYER_MAX_URL (apkrt.h) or neither http nor https, a seek's time
/// below 0 or mode above 3, a value outside its key's range, DATA above the READ's size or above
/// APKRT_MEDIA_PLAYER_MAX_DATA, and RENDER's, RENDER_WINDOW's and DISCARD's as the video decoder's; -EBADF for an ID
/// that is not there, or no more, and for a SOURCE fd the guest does not have; -EBUSY for a CREATE past
/// APKRT_MEDIA_PLAYER_MAX_PLAYERS; -EACCES for a URL source of an app without INTERNET (apkrt_set_internet, apkrt.h),
/// whose sockets answer EPERM; -ENOSPC with the size out for a TRACKS buffer too small. DATA with a token other
/// than the READ that waits answers -EINVAL: a late answer that lost to a seek or a DESTROY, which the guest drops.
/// Else the host's answer. The number follows battery's.
#define APKRT_NR_MEDIA_PLAYER 4132

// MARK: - Audio decoding (the SoundPool plan's step 3)
//
// SoundPool decodes its compressed sounds through the host (AudioToolbox's ExtAudioFile on macOS and iOS): the guest
// hands the file's bytes and a buffer, and gets 16-bit interleaved frames at the file's rate and channels, as the
// decoders of AOSP's Sound.cpp give them, up to the buffer's end (Sound.cpp's heap, which cuts a longer sound). Only
// what Android's SoundPool decodes is taken: MP3, AAC (in MP4 or ADTS) and FLAC; WAV and Ogg Vorbis stay the guest's
// own.

struct apkrt_audio_decode {
    uint64_t data;      // the file's bytes
    uint64_t size;      // 1 to APKRT_AUDIO_DECODE_MAX_FILE
    uint64_t out;       // where the frames go
    uint64_t capacity;  // its bytes; the decode stops at the last whole frame that fits
    uint64_t frames;    // out: the frames written
    uint32_t rate;      // out: the file's sample rate
    uint32_t channels;  // out: and channels
};

/// The largest file audio_decode takes, as SoundPool reads a file whole.
#define APKRT_AUDIO_DECODE_MAX_FILE (64u << 20)

/// audio_decode: x0 a struct apkrt_audio_decode. Decodes the file's frames into `out` until its end or the buffer's,
/// and sets frames, rate and channels. Returns 0; -EFAULT for a struct the guest cannot read or write, bytes it cannot
/// read or a buffer it cannot write; -EINVAL for a size of 0 or above APKRT_AUDIO_DECODE_MAX_FILE, and for a file the
/// host does not decode or that is none of the formats above; -ENOSYS without the host's decoder; -ENOMEM without
/// memory. The number follows media_player's.
#define APKRT_NR_AUDIO_DECODE 4133

// MARK: - Sensors (ADR-0061)
//
// One syscall carries every sensor operation, as the video decoder's does: x0 the op, x1 the argument. The guest opens
// a queue (a read-only pipe whose descriptor it polls on its looper), enables sensor types on it and reads whole
// ASensorEvent records straight from the descriptor; the host (App's CoreMotion) feeds the hub, which fans each sample
// out to every queue that has the type enabled. Closing the descriptor (ASensorManager_destroyEventQueue, or any close)
// stops the queue's sensors. The runtime checks every argument before the hub acts.
#define APKRT_NR_SENSOR 4129

#define APKRT_SENSOR_LIST    0  // x1 the address of an apkrt_sensor_list the runtime fills; the count, or -errno
#define APKRT_SENSOR_OPEN    1  // x1 0; returns a new queue's read descriptor (close-on-exec, non-blocking), or -errno
#define APKRT_SENSOR_ENABLE  2  // x1 the address of an apkrt_sensor_request; begins/re-rates request.type; 0 or -errno
#define APKRT_SENSOR_DISABLE 3  // x1 the address of an apkrt_sensor_request (period ignored); 0 or -errno
#define APKRT_SENSOR_RATE    4  // x1 the address of an apkrt_sensor_request; re-rates an enabled type; 0 or -errno
#define APKRT_SENSOR_FLUSH   5  // x1 a queue's descriptor; a flush-complete event per enabled sensor; 0 or -errno

/// The sensors the runtime can offer (7 always on a modern iPhone, plus the 2 uncalibrated): the length of
/// apkrt_sensor_list.sensors.
#define APKRT_SENSOR_MAX 9
/// The bytes of a string in apkrt_sensor_info, the NUL included.
#define APKRT_SENSOR_NAME_MAX 64

/// One offered sensor as APKRT_SENSOR_LIST gives it; ASensor_* and android.hardware.Sensor read from these. The handle
/// is `type`. The strings are NUL-terminated within their arrays.
struct apkrt_sensor_info {
    int32_t type;            // Android ASENSOR_TYPE_ number, and the sensor's handle
    int32_t value_count;     // floats a sample carries (3 to 6)
    int32_t version;
    int32_t min_delay_us;
    int32_t max_delay_us;
    int32_t reporting_mode;  // an AREPORTING_MODE_ value
    int32_t flags;           // Android's Sensor flags word
    float max_range;
    float resolution;
    float power_ma;
    char name[APKRT_SENSOR_NAME_MAX];
    char vendor[APKRT_SENSOR_NAME_MAX];
    char string_type[APKRT_SENSOR_NAME_MAX];
    char required_permission[APKRT_SENSOR_NAME_MAX];
};

/// APKRT_SENSOR_LIST's buffer: the runtime writes `count` (how many it offers) and that many entries of `sensors`.
struct apkrt_sensor_list {
    uint32_t count;
    uint32_t reserved;
    struct apkrt_sensor_info sensors[APKRT_SENSOR_MAX];
};

/// The argument of APKRT_SENSOR_ENABLE, APKRT_SENSOR_DISABLE and APKRT_SENSOR_RATE.
struct apkrt_sensor_request {
    int32_t queue;      // a queue's descriptor, from APKRT_SENSOR_OPEN
    int32_t type;       // an Android ASENSOR_TYPE_ number
    int64_t period_ns;  // the requested sampling period (ENABLE and RATE); ignored by DISABLE
};

// MARK: - Location (ADR-0063)
//
// One syscall carries every location operation, as the sensor's does: x0 the op, x1 its argument, x2 a sequence for
// GET. A runtime location hub (location_hub.h) like the sensor hub, but delivering through the session event stream
// (APKRT_EVENT_LOCATION), because the consumer is the Java ILocationManager pump, which already reads apkrt_event: a
// fix (about 13 fields) is larger than a 24-byte event, so the event is a signal and the fix is fetched with
// APKRT_LOCATION_GET. The host (App's CoreLocation side) feeds the hub (apkrt_location_push/_push_single in apkrt.h),
// which the runtime gates: the per-group grant the host records through apkrt_permission_answer (ADR-0063 decision 2)
// is the real gate, because iOS's authorisation is for all of Minato, so a guest denied in Minato's sheet must not
// reach the device by calling this syscall. Every data op answers -EACCES without a location grant, and a coarse
// grant makes the runtime fudge every fix it hands the guest (the fudger runs here, never in the guest).
#define APKRT_NR_LOCATION 4130

#define APKRT_LOCATION_START   0  // x1 the address of an apkrt_location_control: begin/reconfigure continuous updates;
                                  // 0, -EFAULT, -EINVAL (bad accuracy), -EACCES (no grant)
#define APKRT_LOCATION_STOP    1  // x1 0: end continuous updates. 0 (never gated, so a guest can always stop)
#define APKRT_LOCATION_CURRENT 2  // x1 a guest token (!=0): getCurrentLocation, one fix for the token, which the
                                  // answer event carries (bit 0). 0, -EINVAL (token 0), -EEXIST (token outstanding),
                                  // -EBUSY (past APKRT_LOCATION_ONESHOT_MAX), -EACCES
#define APKRT_LOCATION_CANCEL  3  // x1 an outstanding token: the CancellationSignal; drops the one-shot and tells the
                                  // host. 0, -EINVAL (token 0), -ENOENT (unknown token)
#define APKRT_LOCATION_LAST    4  // x1 the address of an apkrt_location_fix: the last continuous fix
                                  // (getLastKnownLocation). 1 (filled), 0 (none yet), -EFAULT, -EACCES
#define APKRT_LOCATION_GET     5  // x1 the address of an apkrt_location_fix, x2 a sequence: the fix an event announced.
                                  // 1 (filled), 0 (gone/replaced), -EFAULT, -EACCES
#define APKRT_LOCATION_PERMISSION 6  // a PERMISSION read (checkSelfPermission), not a location op: x1 a
                                  // permission group (APKRT_PERMISSION_*), returns its stored grant normalized to 0
                                  // (denied or not yet answered), 1 (granted/fine) or 2 (granted coarse); -EINVAL for
                                  // an unknown group. NEVER gated: it is how a guest learns it was denied. It answers
                                  // the microphone group too, and only rides APKRT_NR_LOCATION as the next free op.
                                  // The guest must query it at the first checkSelfPermission, not keep a value from
                                  // before the host's answer, then track changes through APKRT_EVENT_PERMISSION_RESULT

/// APKRT_LOCATION_START's and the host's accuracy.
#define APKRT_LOCATION_ACCURACY_FINE   1  // kCLLocationAccuracyBest
#define APKRT_LOCATION_ACCURACY_COARSE 2  // kCLLocationAccuracyReduced; the runtime asks the host for this under a
                                          // coarse grant whatever the guest requested

/// APKRT_EVENT_LOCATION's flags (a1).
#define APKRT_LOCATION_EVENT_ONE_SHOT 1u  // an answer to an APKRT_LOCATION_CURRENT; a2 is its token
#define APKRT_LOCATION_EVENT_COARSE   2u  // the fix is coarse (reduced accuracy)
#define APKRT_LOCATION_EVENT_NO_FIX   4u  // a one-shot resolved with no location; no GET follows

/// At most this many one-shot (getCurrentLocation) requests outstanding at once; a CURRENT past it is -EBUSY. A
/// one-shot entry lives until its GET, its CANCEL, or a no-fix resolve, and a continuous fix never evicts it, so a
/// getCurrentLocation answer is never lost under continuous traffic.
#define APKRT_LOCATION_ONESHOT_MAX 8

/// apkrt_location_fix.valid: which optional fields hold a value (lat, lon and time are always set). CLLocation's
/// negative sentinels become a cleared bit; a coarse fix keeps only latitude, longitude, time and the horizontal
/// accuracy, as Android's coarse Location does.
#define APKRT_LOCATION_HAS_ALTITUDE            1u
#define APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY 2u
#define APKRT_LOCATION_HAS_VERTICAL_ACCURACY   4u
#define APKRT_LOCATION_HAS_SPEED               8u
#define APKRT_LOCATION_HAS_SPEED_ACCURACY      16u
#define APKRT_LOCATION_HAS_BEARING             32u
#define APKRT_LOCATION_HAS_BEARING_ACCURACY    64u

/// APKRT_LOCATION_START's argument: the wanted continuous state.
struct apkrt_location_control {
    int32_t accuracy;    // APKRT_LOCATION_ACCURACY_FINE or _COARSE
    int32_t distance_m;  // the smallest move that makes a new fix (CLLocationManager.distanceFilter), 0 for every fix
};

/// One location fix (APKRT_LOCATION_LAST and _GET fill it; apkrt_location_push carries it). Times: `time_ms` is the
/// CoreLocation timestamp in milliseconds since the epoch; `elapsed_realtime_ns` is on the guest's CLOCK_BOOTTIME /
/// elapsedRealtimeNanos base (the host computes it, the hub passes it through unchanged). `valid` says which optional
/// fields hold a value (APKRT_LOCATION_HAS_*). A fix with a negative horizontal accuracy is invalid and dropped.
struct apkrt_location_fix {
    double latitude;
    double longitude;
    double altitude;                  // ellipsoidal metres (CLLocation.altitude)
    float horizontal_accuracy_m;
    float vertical_accuracy_m;
    float speed_mps;
    float speed_accuracy_mps;
    float bearing_deg;
    float bearing_accuracy_deg;
    int64_t time_ms;
    int64_t elapsed_realtime_ns;
    uint32_t provider;                // a host hint: 3 fused (one CoreLocation source). The framework sets the Java
                                      // Location's provider per the listener from {gps, fused, passive}; "network"
                                      // does not exist on Android 15 (ADR-0063)
    uint32_t valid;                   // APKRT_LOCATION_HAS_* bits
};

// MARK: - Battery (App's gap-1 proposal, 2026-10-29)
//
// The host (App's UIDevice side on iOS, apk_run's fixed model on the Mac) reports the battery with
// apkrt_battery_set (apkrt.h). It stores the value and, while a session runs, posts APKRT_EVENT_BATTERY on a
// change. The Java BatteryManager and the sticky ACTION_BATTERY_CHANGED live on the framework side (App); this is
// only the transport. No permission is needed, as Android's battery broadcast needs none.

/// One syscall carries every battery operation, like the sensor's and the location's: x0 the op.
#define APKRT_NR_BATTERY 4131

#define APKRT_BATTERY_GET 0  // x0 0: the current battery packed as (level & 0xFFFFFFFF) | (state << 32), both
                             // non-negative (an unknown level is 0xFFFFFFFF in the low half, so the packed value is
                             // always positive and never aliases a negative errno); -ENODATA before the host's first
                             // apkrt_battery_set. Never gated (battery needs no permission). The sticky
                             // BATTERY_CHANGED reads it at the first registerReceiver, before any event

/// APKRT_EVENT_BATTERY's state (a1) and APKRT_BATTERY_GET's high half: UIDevice.BatteryState's raw values, so the
/// host passes them through unchanged. The Android mapping (BatteryManager.BATTERY_STATUS_*, plugged, battery-low) is
/// the framework's (App).
#define APKRT_BATTERY_STATE_UNKNOWN   0
#define APKRT_BATTERY_STATE_UNPLUGGED 1
#define APKRT_BATTERY_STATE_CHARGING  2
#define APKRT_BATTERY_STATE_FULL      3

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

/// The kinds of record an input channel carries (ADR-0068, step 0). input_read fills one buffer of
/// sizeof(struct apkrt_motion_event) bytes whatever the kind; the kind is the uint32_t at byte 12 of every record,
/// 0 for a motion event, so that a reader that knows only motion events sees the others as not its own. A reader
/// that does not handle a kind drops its records.
#define APKRT_INPUT_MOTION 0
#define APKRT_INPUT_KEY 1
#define APKRT_INPUT_TEXT 2
#define APKRT_INPUT_DEVICE 3

/// A motion event of the touchscreen as input_read gives it (AMotionEvent). Its source, tool type and device
/// are libandroid's in M3: AINPUT_SOURCE_TOUCHSCREEN, AMOTION_EVENT_TOOL_TYPE_FINGER and one device.
struct apkrt_motion_event {
    int32_t action;          // AMOTION_EVENT_ACTION_*: DOWN 0, UP 1, MOVE 2, CANCEL 3, and POINTER_DOWN 5 and
                             // POINTER_UP 6 with the pointer's index << 8
    uint32_t pointer_count;  // 1 to APKRT_MOTION_POINTERS
    uint32_t sample_count;   // 1 to APKRT_MOTION_SAMPLES: the history, oldest first, then the event's own sample
    uint32_t kind;           // APKRT_INPUT_MOTION
    int64_t down_time;       // the time of the gesture's DOWN
    int32_t pointer_ids[APKRT_MOTION_POINTERS];  // in the order of the pointers: ascending, 0 to 31
    int64_t sample_times[APKRT_MOTION_SAMPLES];  // the last is the event's time
    struct apkrt_pointer_coords coords[APKRT_MOTION_SAMPLES][APKRT_MOTION_POINTERS];  // by sample, then pointer index
};

/// A key (AKeyEvent) as input_read gives it, after the motion event's four header words.
struct apkrt_key_event {
    int32_t action;       // AKEY_EVENT_ACTION_DOWN 0 or UP 1
    uint32_t reserved0[2];
    uint32_t kind;        // APKRT_INPUT_KEY
    int64_t down_time;    // the time of the key's DOWN
    int64_t event_time;
    int32_t key_code;     // AKEYCODE_*, 0 or more
    int32_t scan_code;
    int32_t meta_state;   // AMETA_*
    int32_t repeat_count;
    int32_t device_id;
    int32_t flags;        // AKEY_EVENT_FLAG_*
    int32_t source;       // AINPUT_SOURCE_*
    int32_t reserved1;
};

/// The longest text of one record, in bytes of UTF-8.
#define APKRT_TEXT_BYTES 1024

/// What a text record asks of the focused view's InputConnection (ADR-0068, part 1).
#define APKRT_TEXT_COMMIT 0             // commitText(text, arg0 = newCursorPosition)
#define APKRT_TEXT_SET_COMPOSING 1      // setComposingText(text, arg0 = newCursorPosition)
#define APKRT_TEXT_FINISH_COMPOSING 2   // finishComposingText()
#define APKRT_TEXT_DELETE_SURROUNDING 3 // deleteSurroundingText(arg0 = before, arg1 = after), in UTF-16 units
#define APKRT_TEXT_SET_SELECTION 4      // setSelection(arg0 = start, arg1 = end)
#define APKRT_TEXT_SET_COMPOSING_REGION 5  // setComposingRegion(arg0 = start, arg1 = end)
#define APKRT_TEXT_PERFORM_ACTION 6     // performEditorAction(arg0 = IME_ACTION_*)
#define APKRT_TEXT_SEND_KEY 7           // sendKeyEvent: a DOWN and an UP of key code arg0, from the soft keyboard
#define APKRT_TEXT_OPS 8

/// A text edit of the host's keyboard, for the connection with `serial`: the host takes the serial from the
/// editor it was told of, so that a record for a connection that has since changed is dropped by the guest.
struct apkrt_text_event {
    int32_t op;           // APKRT_TEXT_*
    int32_t arg0;
    int32_t arg1;
    uint32_t kind;        // APKRT_INPUT_TEXT
    int64_t serial;
    uint32_t length;      // the bytes in text, 0 to APKRT_TEXT_BYTES; no NUL is needed
    uint32_t reserved;
    char text[APKRT_TEXT_BYTES];
};

/// What a device record says (ADR-0068, part 3).
#define APKRT_DEVICE_ADDED 0
#define APKRT_DEVICE_REMOVED 1
#define APKRT_DEVICE_CHANGED 2
#define APKRT_DEVICE_OPS 3

/// An input device that came, went or changed.
struct apkrt_device_event {
    int32_t op;           // APKRT_DEVICE_*
    int32_t device_id;    // the host's, 1 or more; 0 and -1 are Android's own
    int32_t sources;      // InputDevice.SOURCE_* of the device
    uint32_t kind;        // APKRT_INPUT_DEVICE
    int32_t vendor_id;
    int32_t product_id;
    char name[64];        // UTF-8 with its NUL: valid, nothing above U+FFFF, no NUL inside (Java takes it as it is)
};
// The host posts a device record to one channel, the first window's, and the guest ignores a record that repeats the
// device it has.

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

/// The ring holds this many bursts (the capacity); the guest fills this many at first (the buffer size) of a
/// LOW_LATENCY stream, as Android's MMAP streams start. Any other stream starts at the capacity, as the legacy
/// AudioTrack-backed streams do (AudioStreamTrack::getBufferSize is the track's frame count at the open).
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
    int32_t buffer_size;       // out: APKRT_AUDIO_FIRST_BURSTS bursts for LOW_LATENCY, else the capacity
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

/// In libandroid.so, version APKRT_PRIVATE: the asset manager of the `count` APKs at the guest paths `apks`
/// (ADR-0028, decision 10), for ANativeActivity: base.apk first, then the splits of an installed set (ADR-0048
/// decision 9), of which the last is read first, as Android reads a set. It reads each ZIP's central directory once
/// and keeps a descriptor of the APK; AConfiguration_fromAssetManager gives the values decision 9 chooses from
/// `configuration`, the session's. NULL when an APK cannot be read or its central directory is not sound, or
/// without memory.
struct AAssetManager *minato_AAssetManager_create(const char *const *apks, size_t count,
                                                  const struct apkrt_configuration *configuration);

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

/// In libaaudio.so, from the stream's data callback: only the first `frames` of this call's frames are audio, and
/// the stream's thread hands over just those, where it would hand over the whole call with the rest as the callback
/// left it. A source that has fewer frames than a call asks for (libOpenSLES, whose buffer queue holds what the app
/// enqueued so far) says so instead of padding with zeros, which the ring would play as silence at a time the app's
/// next buffer may be about to fill (SDL's OpenSL ES driver at 512-frame buffers lost 20 % of its frames to that).
/// With 0 the thread waits a millisecond before it asks again. From another thread it does nothing.
void minato_AAudioStream_setCallbackFrames(struct AAudioStreamStruct *stream, int32_t frames);

#ifdef __cplusplus
}
#endif

#endif
