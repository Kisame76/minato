/* graphics/host's work sources for the runtime's stall watchdog (apkrt_stall_set_work_source and
   apkrt_stall_set_return_work_source, apkrt.h), from ANGLE's own memory counters (patch 0015,
   ANGLEMetalMemoryCounters): the bytes of the textures and buffers ANGLE holds in Metal, relaxed atomic loads.
   - Work toward a first frame: the most of those bytes seen, so a loop that uploads into the same textures again
     stays flat, plus the runtime's asset work (apkrt_stall_asset_work).
   - Work after a return: the sum of their rises from one pull to the next, so a reload that deletes its textures and
     makes them again grows it, plus the runtime's cumulative asset reads (apkrt_stall_asset_read_cumulative).
   The texture and buffer part counts for a window that had an EGL window surface (angle.m names each one); any other
   surface (a Vulkan one, a Java app's window) gets the asset part alone, as from the runtime's own sources.
   Limits: the counters are process-wide, so two GL windows get the same part; a rise that falls back between two
   pulls (the watchdog's tick, 1 s) is not seen; an upload into a texture that has its storage already moves no bytes
   ANGLE holds; ANGLEMetalMemoryCounters also reads the device's currentAllocatedSize, a property read.
   Registered once, at the first EGL window surface, when ANGLE has the counters; without them (an ANGLE before patch
   0015) the runtime's sources stay. Both sources are lock-free and call nothing that locks: the watchdog pulls them
   under its own lock, one at a time. */
#include "stall_work.h"

#include <pthread.h>
#include <stdatomic.h>

#include "APKRuntime/apkrt.h"

/* The counts and bytes of patch 0015's classes: compressed, color, depth/stencil, multisample and memoryless
   textures, shared and private buffers (values 0 to 13); the bytes are the odd ones. */
enum { class_values = 14 };

static size_t (*_Atomic counters)(uint64_t *values, size_t count);

/* The windows that had an EGL window surface: a ring written at each surface's creation, read without a lock. A
   session has a few; past 64 the oldest is written over. */
enum { gl_window_slots = 64 };
static _Atomic uint64_t gl_windows[gl_window_slots];
static atomic_uint gl_window_next;

static _Atomic uint64_t most;     /* the most live bytes seen */
static _Atomic uint64_t last;     /* the live bytes at the last pull */
static _Atomic uint64_t risen;    /* the sum of the rises between pulls */

static uint64_t live_bytes(void) {
    size_t (*const read)(uint64_t *, size_t) = atomic_load_explicit(&counters, memory_order_acquire);
    if (read == NULL) {
        return 0;
    }
    uint64_t values[class_values] = {0};
    read(values, class_values);
    uint64_t bytes = 0;
    for (size_t k = 1; k < class_values; k += 2) {
        bytes += values[k];
    }
    return bytes;
}

static int gl_window(uint64_t id) {
    if (id == 0) {
        return 0;
    }
    for (size_t k = 0; k < gl_window_slots; ++k) {
        if (atomic_load_explicit(&gl_windows[k], memory_order_relaxed) == id) {
            return 1;
        }
    }
    return 0;
}

/* Reads the live bytes: raises the most seen, and adds the rise since the last pull. */
static void pull(void) {
    const uint64_t live = live_bytes();
    uint64_t seen = atomic_load_explicit(&most, memory_order_relaxed);
    while (live > seen &&
           !atomic_compare_exchange_weak_explicit(&most, &seen, live, memory_order_relaxed, memory_order_relaxed)) {
    }
    const uint64_t before = atomic_exchange_explicit(&last, live, memory_order_relaxed);
    if (live > before) {
        atomic_fetch_add_explicit(&risen, live - before, memory_order_relaxed);
    }
}

uint64_t minato_graphics_stall_work(uint64_t id) {
    uint64_t work = apkrt_stall_asset_work();
    if (gl_window(id)) {
        pull();
        work += atomic_load_explicit(&most, memory_order_relaxed);
    }
    return work;
}

uint64_t minato_graphics_stall_return_work(uint64_t id) {
    uint64_t work = apkrt_stall_asset_read_cumulative();
    if (gl_window(id)) {
        pull();
        work += atomic_load_explicit(&risen, memory_order_relaxed);
    }
    return work;
}

void minato_graphics_stall_counters(size_t (*read)(uint64_t *values, size_t count)) {
    atomic_store_explicit(&counters, read, memory_order_release);
}

static pthread_once_t registered = PTHREAD_ONCE_INIT;

static void register_sources(void) {
    apkrt_stall_set_work_source(minato_graphics_stall_work);
    apkrt_stall_set_return_work_source(minato_graphics_stall_return_work);
}

void minato_graphics_stall_gl_window(uint64_t window) {
    if (window == 0) {
        return;
    }
    if (!gl_window(window)) {
        const unsigned slot = atomic_fetch_add_explicit(&gl_window_next, 1u, memory_order_relaxed) % gl_window_slots;
        atomic_store_explicit(&gl_windows[slot], window, memory_order_relaxed);
    }
    if (atomic_load_explicit(&counters, memory_order_acquire) != NULL) {
        pthread_once(&registered, register_sources);
    }
}
