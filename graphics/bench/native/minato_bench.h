/* MinatoBench (ADR-0037, decision 1): the parity benchmark's scene built for the host, the twin of the APKs
   bench-gles and bench-vulkan. The same scene files draw with ANGLE and MoltenVK directly, without Minato's layer.
   A render thread draws one frame per tick that the caller's display link passes in; the run ends by itself after
   bench.conf's seconds from its first frame and writes scene.csv. One run per process. */
#ifndef MINATO_BENCH_H
#define MINATO_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Starts a run of `api` ("gles" or "vulkan") on `layer`, a CAMetalLayer that the run retains until it ends,
/// at the layer's size in pixels. `conf` is bench.conf's text (ASCII key=value lines: run, rate, load,
/// seconds); `moltenvk` is the path MoltenVK is opened from (the Vulkan scene only); scene.csv goes to
/// `scene_csv` when the run ends. The render thread is at QoS user-interactive, or at default when the process
/// was launched with -benchmarkRenderQoS default. 0, or -1 with the reason in `error` (a second run, a bad
/// argument, an unknown key in `conf`, another value of -benchmarkRenderQoS).
int minato_bench_start(const char *api, const char *conf, void *layer, const char *moltenvk, const char *scene_csv,
                       char *error, size_t size);

/// A tick of the display link at `time` (host time in ns, CLOCK_UPTIME_RAW): the render thread draws a frame
/// for the latest tick it has not drawn.
void minato_bench_tick(int64_t time);

/// The frames drawn so far.
uint64_t minato_bench_frames(void);

/// Waits up to `seconds` for the end of the run: 0 once it ended and wrote scene.csv, 1 while it still runs,
/// -1 when it failed, with the reason in `error`.
int minato_bench_wait(double seconds, char *error, size_t size);

/// The meshes a load draws with an API, the N of run.csv (ADR-0037, decision 7): 0 for an unknown API or load.
int minato_bench_meshes(const char *api, const char *load);

#ifdef __cplusplus
}
#endif

#endif
