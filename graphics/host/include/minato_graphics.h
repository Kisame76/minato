/* graphics/host: the host side of Minato's graphics for the app and the tests on the Mac (ADR-0029,
   ADR-0030). Plain C. */
#ifndef MINATO_GRAPHICS_H
#define MINATO_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the providers of the front libs' host tables with the runtime (ADR-0030, decision 3), before the
   program starts: "vulkan" opens MoltenVK at `moltenvk`, "gles" and "egl" open ANGLE's libGLESv2 at
   `angle`, which holds EGL's functions as EGL_<name>. A provider fills its table from the library's
   exports; a name the library lacks gets a function that logs once and answers "not supported". A NULL
   path leaves its libs without a provider, so they answer "no driver" (the Simulator for Vulkan). With
   MoltenVK, it also sets MVK_CONFIG_LOG_LEVEL to warnings unless the environment has it, graphics/host's
   own functions stand in front of the MoltenVK functions that libvulkan's submits, surfaces, swapchains,
   acquires, presents, past presentation timings, devices and debug callbacks reach (ADR-0030, decisions 5, 12
   and 13; ADR-0041, decision 11), and minato_graphics_vulkan_scheduled becomes a provider of the runtime's
   "scheduled". With ANGLE, its own functions stand in front of the ANGLE functions that libEGL's window
   surfaces, swaps and presentation times and libGLES's debug callbacks reach, ANGLE's commit gate holds commits in the background (ADR-0029, decisions 14 to 16), and
   ANGLE's wait until its commits are scheduled becomes a provider of "scheduled" too. The windows' buffers for
   drawing by the CPU come with them (minato_graphics_set_window_buffers below), and the guest's hardware buffers
   (minato_graphics_set_hardware_buffers).
   Returns 0, or -1 when a path is longer than 1023 bytes or the runtime refused a provider. */
int minato_graphics_set_providers(const char *moltenvk, const char *angle);

/* ADR-0030 V3's check for the iPhone, host code only: opens MoltenVK at `moltenvk` as the "vulkan"
   provider does, creates an instance at Vulkan 1.3 with VK_KHR_surface and VK_EXT_metal_surface and a
   Metal surface on a CAMetalLayer, prints what they report to stdout and destroys both. Returns 0, or 1
   after printing what failed to stderr. Its own object file: project.yml links it into debug builds
   only, and there, with MINATO_GRAPHICS_PROBE set, it runs before main on the MoltenVK in the app's
   Frameworks and the app exits with its result. */
int minato_graphics_probe(const char *moltenvk);

/* ADR-0044 a)'s probe, host code only: whether the last vertex provokes in a Metal render pass set up as MoltenVK
   sets it up with Apple's private API, OpenGL mode on and setProvokingVertexMode: with MoltenVK's value for the last
   vertex, each called only when Metal responds to it, as MoltenVK does. It draws one triangle whose flat color
   differs per vertex four times (neither call, the mode only, OpenGL mode only, both) and prints to stdout which
   vertex's color it read back. Returns 0 when both give the last vertex and neither gives the first, else 1 after
   printing why. A debug build of the app, launched with MINATO_PROVOKING_PROBE set, runs it once it is active
   (probe.m) and exits with its result. */
int minato_provoking_probe(void);

/* ADR-0044, decision 4: the backend under the session's ANGLE, set before the app starts: 0 Metal, the default, or 1
   ANGLE's Vulkan backend on MoltenVK, the ES 3.1 path (decision 3), for an app whose manifest requires ES 3.1. On
   the Vulkan path libEGL's display is ANGLE's Vulkan display, and ANGLE reaches MoltenVK only through graphics/host's
   functions (angle_vulkan.m): its instances with MoltenVK's private API on (decision 2), its devices with
   VK_EXT_metal_objects and their queues in "scheduled", its submits held in the background as the commit gate holds
   Metal's, and its acquires and presents with VK_SUBOPTIMAL_KHR as VK_SUCCESS. ro.opengles.version follows the
   backend, ES 3.1 on Vulkan and ES 3.0 on Metal (apkrt_set_opengles_version), so it is set before the program
   starts. Returns 0, or -1 when there is no MoltenVK for ANGLE (minato_graphics_set_providers without its path)
   or ANGLE lacks patch 0009's setter. */
int minato_graphics_set_angle_backend(int vulkan);

/* ADR-0044, decision 1, at run time too: whether the ES 3.1 path may run, before ANGLE's instance is made. Draws
   a)'s triangle quietly in a Metal pass as MoltenVK sets it up, once without and once with both of Apple's calls.
   Returns 1 when the first vertex provokes without them and the last with them, with the time it took in `reason`
   ("passed in 2.41 ms", `size` bytes); 0 otherwise with the reason there, and also when `forced` asks it to fail, as
   the App's Debug override does (test 2). Its shader is compiled from source until c2b measures that time
   (a departure from decision 1 for now). */
int minato_graphics_angle_vulkan_check(int forced, char *reason, size_t size);

/* For tests: the swapchains ANGLE's Vulkan backend made through graphics/host since the program started. */
unsigned minato_graphics_angle_vulkan_swapchains(void);

/* ADR-0041 decision 11 c)'s probe of the drawables' pool, host code only: whether Core Animation takes a drawable's
   surface back into the layer's pool only once the drawable is let go, as MoltenVK lets an image's drawable go when it
   acquires that image again. On a layer of its own over the app's window, frames paced by a display link at 60 and then
   120 Hz each clear a drawable and present it: for `seconds` per run, letting each drawable go at once, then holding
   it 3 frames as MoltenVK does. For each run it prints to stdout how long nextDrawable waited (p50, p99, and how many
   waits were longer than half a period) and how far apart the ticks were. Returns 0, or 1 after printing why it could
   not run. A debug build of the app, launched with MINATO_DRAWABLE_PROBE set, runs it for 3 s per run once it is
   active (probe.m) and exits with its result. */
int minato_drawable_probe(double seconds);

/* "Scheduled" for Vulkan (ADR-0030, decision 13), from a host thread, which need not be the main thread:
   waits until no call of libvulkan that makes MoltenVK commit is in flight, then commits one empty command
   buffer on each queue of every device libvulkan made and waits until Metal has scheduled them, which it
   does in each queue's order, after every command buffer committed there before. `deadline` is in
   nanoseconds of CLOCK_UPTIME_RAW (clock_gettime_nsec_np). Returns 1 once all are scheduled, 0 when the
   deadline came first. */
int minato_graphics_vulkan_scheduled(uint64_t deadline);

/* The windows' buffers for drawing by the CPU (ADR-0043, decision 3; window.m): gives the runtime graphics/host's
   buffers (apkrt_set_window_buffers) and their provider of "scheduled". minato_graphics_set_providers calls it.
   Returns 0, or -1 when the runtime refused either. */
int minato_graphics_set_window_buffers(void);

/* "Scheduled" for the windows' buffers: waits until no post is between its count and its commit, then until Metal
   has scheduled an empty command buffer on their queue, and so every one committed there before. Returns 1 once
   scheduled, at once before the first lock, 0 when `deadline` (as for Vulkan's above) came first. */
int minato_graphics_window_scheduled(uint64_t deadline);

/* For tests and device probes (ADR-0041 decision 11 c)): the window's present that its app asked for at `asked`, in
   nanoseconds of the uptime clock, is not shown, as one the rule drops. Returns 0 once noted, -1 when the window's
   layer is no MinatoMetalLayer or 8 presents are noted already. */
int minato_graphics_drop_present(uint64_t window, int64_t asked);

/* ADR-0058 a): graphics/host's hardware buffers (hardware_buffer.m), the memory of the guest's AHardwareBuffers as
   IOSurfaces in the process, which the guest reads and writes at a lock's address: gives them to the runtime
   (apkrt_set_hardware_buffers). minato_graphics_set_providers calls it. Returns 0, or -1 when the runtime refused
   them. */
int minato_graphics_set_hardware_buffers(void);

struct apkrt_hardware_buffers;

/* For tests: the functions minato_graphics_set_hardware_buffers gives the runtime. */
const struct apkrt_hardware_buffers *minato_graphics_hardware_buffers(void);

/* ADR-0060 e2b3: graphics/host's video decoders (video_decoder.m), VideoToolbox's, which decode the guest's H.264 and
   HEVC into NV12 frames: gives them to the runtime (apkrt_set_video_decoders). minato_graphics_set_providers calls
   it. Returns 0, or -1 when the runtime refused them. */
int minato_graphics_set_video_decoders(void);

struct apkrt_video_decoders;

/* For tests: the functions minato_graphics_set_video_decoders gives the runtime. */
const struct apkrt_video_decoders *minato_graphics_video_decoders(void);

/* ADR-0060 e2e1: graphics/host's media players (media_player.m), AVPlayer's, which play the guest's MediaPlayer
   sources: gives them to the runtime (apkrt_set_media_players). minato_graphics_set_providers calls it. Returns 0,
   or -1 when the runtime refused them. */
int minato_graphics_set_media_players(void);

struct apkrt_media_players;

/* For tests: the functions minato_graphics_set_media_players gives the runtime. */
const struct apkrt_media_players *minato_graphics_media_players(void);

struct apkrt_webview_call;

/* ADR-0056 c1: graphics/host's WebViews (webview.m), WKWebView's: the runtime's WebView handler
   (apkrt_session.webview), which takes each request of the guest at once and carries it out on the main queue, whose
   run loop must run. `context` is unused. */
void minato_webview_handle(void *context, const struct apkrt_webview_call *call);

/* ADR-0056 c2: where the WebViews' views go, the host's windows. Both functions run on the main queue. */
struct minato_webview_windows {
    void *context;
    /* The view (a UIView, an NSView on the Mac) whose subviews window `window`'s WebViews join, or NULL for a window
       the host does not show; `*above` is the window's own view among them, which they go just below, and `*scale`
       the window's pixels per point. */
    void *(*container)(void *context, uint64_t window, void **above, double *scale);
    /* The count of WebViews `window` shows changed: while it is not 0, the window's layer must let them through
       where the guest cleared its pixels (not opaque, no background). */
    void (*shown)(void *context, uint64_t window, uint32_t count);
};

/* Sets the host's windows, or NULL for none: then no view is shown. `windows` must stay valid. */
void minato_webview_set_windows(const struct minato_webview_windows *windows);

/* The host's windows moved or scaled (a fold, a letterbox) without a new PLACE: every view is placed again. */
void minato_webview_windows_changed(void);

/* For b)'s EGL images, e1)'s Vulkan imports and tests: the IOSurfaceRef of one of their handles, retained for the
   caller, who releases it, or NULL for a handle they do not have. */
void *minato_graphics_hardware_buffer_surface(uint64_t handle);

#ifdef __cplusplus
}
#endif

#endif
