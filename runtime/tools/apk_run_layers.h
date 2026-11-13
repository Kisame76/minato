// apk-run's layers (ADR-0043 c4 (c), ADR-0041 (c)): the hooks that graphics-run's session layers
// (graphics/tests/session_layers.m) give apk-run's --lifecycle, so that the session's window, its surfaces and the
// guest's requested windows get layers, as the App's session view gives them; apk-run alone has none, and its windows
// no layer. C, for apk_run.cpp and the session layers alike.
#pragma once

#include "APKRuntime/apkrt.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Every hook runs on a stack that may be small and must not block. The surface hooks run on a guest thread with the
/// runtime signal blocked, under the runtime's surface lock and the headless host's. The requested windows' run the
/// same way, under the runtime's window lock and the headless host's. window_opaque runs in the request handler.
/// window_layer runs on apk-run's lifecycle thread. So do surface_removed and requested_removed at the background and
/// the end, under the headless host's lock alone. None waits for another thread: a hook makes the layer it returns
/// where it is asked, and changes the tree (a layer's place in it, its frame, z, opacity and dim) on a queue of the
/// host's own, in the order asked.
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
    /// A requested window's layer (ADR-0041: a dialog's, a popup's, a toast's, a translucent activity's or a
    /// sub-window's) at the request's frame, opaque as the request says, with its alpha as the layer's opacity, in
    /// Android 15's order of windows (the plan's decision 1), with the dim of the topmost dimming window; the runtime
    /// retains it for its window.
    void *(*requested_layer)(const struct apkrt_window_request *request);
    /// Each place of a live requested window: its frame, alpha and dim.
    void (*requested_placed)(const struct apkrt_window_request *request);
    /// A requested window's layer after its window's removal, as surface_removed has it.
    void (*requested_removed)(void *layer);
};

/// The window host's hooks; NULL in apk-run alone.
const struct apk_run_layer_hooks *apk_run_layers(void);

#ifdef __cplusplus
}
#endif
