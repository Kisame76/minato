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

/// The private syscalls of ADR-0028 (decision 11) for activity-host, libandroid and the front libs,
/// through the veneer like every syscall: the session's connect, answers and requests (decisions 2 to
/// 4) and the window's gate (decision 7). Their arguments and records come with the session (ADR-0028,
/// step c); until then the runtime answers -ENOSYS.
#define APKRT_NR_SESSION_CONNECT 4102
#define APKRT_NR_SESSION_ANSWER 4103
#define APKRT_NR_SESSION_REQUEST 4104
#define APKRT_NR_WINDOW_GATE 4105

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

/// In libnativewindow.so, version APKRT_PRIVATE: a new window of the host window `id`, `width` × `height`
/// pixels in the format WINDOW_FORMAT_RGB_565, as NativeActivity's window starts, with one reference
/// for the caller (activity-host, at NATIVE_WINDOW_CREATED). NULL without memory.
struct ANativeWindow *minato_ANativeWindow_create(uint64_t id, int32_t width, int32_t height);

#ifdef __cplusplus
}
#endif

#endif
