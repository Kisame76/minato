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
                                          // is made anew. A name is therefore new unless it is the same document.
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
                                          // host holds the app's version. tools/pick_host.h's WriteBack is the
                                          // reference and tests/pick_host_test.cpp plays the cases.
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
                                   // than 0 only for kind 5; up to 8 MIME types of at most 100 bytes each, separated
                                   // by newlines, none is allowed, and none for kind 4; a title of at most 200 bytes
                                   // of UTF-8 without control, separator or formatting characters, only for kind 3;
                                   // at most 1535 bytes in all. At most 4 requests wait at once (-EBUSY), and an ID
                                   // that waits is refused (-EEXIST). The PICK and EXPORT_MEDIA requests reach the
                                   // host's handler one at a time; requests of other types may reach it meanwhile. The
                                   // host answers once with APKRT_EVENT_PICK_RESULT, which ends the request
    APKRT_REQUEST_PICK_CANCEL,     // guest only: a0 the ID of a request that waits and the guest no longer wants (its
                                   // activity is gone); the host gets the ID as `argument` and no text, and gives no
                                   // answer. -ENOENT for an ID that does not wait
    APKRT_REQUEST_OPEN,            // guest only (ADR-0062): a0 the address of NUL-terminated text, a1 the request's ID
                                   // (not 0, not one that waits); the host gets the ID as `argument` and the text as
                                   // `text`: "<kind>\t<field>\t<field>...", each field percent-encoded UTF-8 (RFC
                                   // 3986's unreserved bytes raw and never as %XX, every other byte as %XX), an empty
                                   // field being "". Kinds: 1 VIEW, one field, an absolute URL of scheme http, https, tel or maps;
                                   // 2 MAIL, one field, a mailto: URL (RFC 6068: to in the path, cc, bcc, subject and
                                   // body in the query); 3 SMS, one field, an sms: URL; 4 SHARE, 3 + 2n fields: the
                                   // subject, the text, n in decimal without leading zeros (0 to 10), n names of the
                                   // files the guest put into ".minato/share/<ID in decimal>" below the app's data
                                   // folder before the request, then n MIME types, one per name. The runtime refuses
                                   // (-EINVAL) any text but this: a kind 1 to 4 and its number of fields; every %
                                   // followed by two hex digits; each field valid UTF-8 once decoded, without NUL; a
                                   // URL field of 1 to 4096 bytes decoded, without control characters, spaces (any of
                                   // Unicode's Zs), line or paragraph separators or format characters (Cf), whose
                                   // scheme (up to the first ":", ASCII case ignored) is the kind's; a subject of at
                                   // most 1000 bytes and a text of at most 65536 bytes decoded, any code point but NUL;
                                   // a name of 1 to 255 bytes decoded, without "/" and the URL's characters but
                                   // U+0020, not "." or "..", each once byte by byte; a MIME type of 3 to 100 bytes
                                   // "<type>/<subtype>" of RFC 6838's characters (or "*", as image/*); at most 204800
                                   // bytes in all. At most 4 requests wait at once (-EBUSY), and an ID that waits is
                                   // refused (-EEXIST). The requests reach the host's handler one at a time. The host
                                   // answers once with APKRT_EVENT_OPEN_RESULT, which ends the request. The host checks
                                   // again and is the last word: it opens only http, https, mailto, tel, sms and maps,
                                   // and nothing while the session is in the background
    APKRT_REQUEST_OPEN_CANCEL,     // guest only: a0 the ID of an open request that waits and the guest no longer wants
                                   // (its activity is gone); the host gets the ID as `argument` and no text, drops what
                                   // it shows for it and gives no answer. -ENOENT for an ID that does not wait
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
};

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
/// format the host does not draw into. Else the host's answer. The number follows hardware_buffer's.
#define APKRT_NR_VIDEO_DECODER 4128

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

#ifdef __cplusplus
}
#endif

#endif
