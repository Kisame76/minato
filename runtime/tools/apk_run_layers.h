// apk-run's layers (ADR-0043 c4 (c)): the hooks that graphics-run's window host (graphics/tests/window_host.m) gives
// apk-run's --lifecycle, so that the session's window and its surfaces get layers, as the App's session view gives
// them; apk-run alone has none, and its windows no layer. C, for apk_run.cpp and the window host alike.
#pragma once

#include "APKRuntime/apkrt.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Every hook runs on a guest thread with the runtime signal blocked, on a stack that may be small: the surface hooks
/// under the runtime's surface lock and the headless host's, window_opaque in the request handler, which must not
/// block. None waits for another thread: a hook makes the layer it returns where it is asked, and changes the tree
/// (a layer's place in it, its frame and its z) on a queue of the host's own, in the order asked.
struct apk_run_layer_hooks {
    /// The session window's layer, at the start and at each return: the same layer every time, which the host keeps;
    /// the runtime retains it for each window it is given to and releases it at that window's removal.
    void *(*window_layer)(int32_t width, int32_t height);
    /// APKRT_REQUEST_WINDOW_OPAQUE's report, 1 or 0, which the window's layer takes before the hook returns, so that
    /// the window's next post sees it (graphics/host's hide rule); the layer starts opaque, as the session does.
    void (*window_opaque)(int opaque);
    /// A new surface's layer at the request's frame, z and opacity, which the runtime retains for its window.
    void *(*surface_layer)(const struct apkrt_surface_request *request);
    /// Each place of a live surface, a move too: its frame and z.
    void (*surface_placed)(const struct apkrt_surface_request *request);
    /// A surface's layer after its window's removal, at the guest's remove, the background or the end: the reference
    /// apkrt_window_layer gave out before the removal, which the hook releases once. NULL for a window without one.
    void (*surface_removed)(void *layer);
};

/// The window host's hooks; NULL in apk-run alone.
const struct apk_run_layer_hooks *apk_run_layers(void);

#ifdef __cplusplus
}
#endif
