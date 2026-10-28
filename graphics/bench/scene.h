/* The parity benchmark's scene (ADR-0037, decision 1), shared by the APKs (android/main.c) and the host build
   (native/native.m). This part is the API's rest: bench.conf, the loads, the motion by the frame index alone, the
   sphere and the texture, the frames' records for scene.csv and the CRC of a frame. What the renderers of gles.c and
   vulkan.c draw into an offscreen target at the window's size:
   - a full-screen background, a gradient that moves with the frame;
   - N lit, textured spheres of 768 triangles, one draw each with its own uniforms, the texture at one level per
     sphere from its size on the window (the shaders' formula), not by derivatives, which vary at triangle edges
     with the GPU's state;
   - N alpha-blended particles in one instanced draw;
   then a full-screen post pass from the target into the window. Every value a pixel depends on comes from + - * /,
   floor and sqrt, without contraction into fused multiply-adds (-ffp-contract=off), so that both builds, with
   bionic's libm and Apple's, draw the same bits: sin and cos are the scene's own. */
#ifndef BENCH_SCENE_H
#define BENCH_SCENE_H

#include <stddef.h>
#include <stdint.h>

enum bench_api { BENCH_GLES, BENCH_VULKAN };
/* BENCH_FILL, M6's MetalFX plan, step 0: no meshes, but full-screen alpha layers of long fragment shaders, a load
   that grows with the pixels (the heavy load is bound by its geometry). */
enum bench_load { BENCH_LIGHT, BENCH_HEAVY, BENCH_FILL };

/* bench.conf (ADR-0037): ASCII key=value lines. run is the run's ID, rate 60 or 120 Hz, load light or heavy,
   seconds the time to draw from the first frame; check, for tests only, the frame whose CRC the scene logs, at its
   place and drawn once before the first frame, while the GPU is cold; scale, 25 to 100 percent, the offscreen target's
   size per side against the window's, which the post pass stretches over the window (M6's MetalFX plan, step 0:
   whether a scene is bound by its pixels); cap, the targetFrameRate an engine's script sets (ADR-0066 step 4), -1 or 1
   to 1000, with which the scene draws only on the ticks the engine's limiter takes; unlock, 60 or 120, the FPS unlock,
   which only the host build applies itself, as the loader's wrapper raises the APK's cap. Without cap the scene draws
   on every tick. */
struct bench_conf {
    char run[64];
    int rate;
    enum bench_load load;
    int seconds;
    int check;  // -1 for none
    int scale;  // percent, 100 by default
    int cap;     // 0 for none
    int unlock;  // 0 for none
};

/* Without bench.conf, the emulator's defaults: 60 Hz, light, 10 s. */
void bench_conf_defaults(struct bench_conf *conf);

/* bench.conf's text over the defaults: 0, or -1 with the line it does not take in `error`. */
int bench_conf_parse(const char *text, struct bench_conf *conf, char *error, size_t size);

/* The offscreen target's size for a window of width x height at `scale` percent: each side rounded to the nearest
   pixel, at least 1. */
void bench_target_size(int width, int height, int scale, int *target_width, int *target_height);

/* The rate an engine draws at for the targetFrameRate `target` it holds: Unity's 30 frames per second on Android and
   iOS for -1 (any value up to 0), else the target. */
int bench_cap_rate(int target);

/* The target the loader's wrapper (runtime/android/frame_rate.c) hands the engine for `cap` under `unlock`, 0 for none:
   -1 and a cap below the unlock become the unlock, any other cap stays. */
int bench_cap_raise(int cap, int unlock);

/* Whether a tick `elapsed` ns after the last drawn frame's tick, with ticks `period` ns apart, is one the engine's
   limiter draws at `rate`: once the rate's interval has passed, within half a period, as a swap interval of whole
   periods takes it. Both builds pass bench.conf's rate's period, not the time between two ticks, so that a tick the
   twin's render thread passes over does not move the next drawn frame. A rate that divides no display rate draws at
   the nearest whole number of periods: on 120 Hz, 90 on every tick and 45 on every 3rd. Whether Unity's limiter or
   Swappy rounds such a rate to the nearest or up is not verified; the tested caps are divisors of 120. */
int bench_cap_due(int64_t elapsed, int64_t period, int rate);

/* N: the meshes and the particles of a load with an API, calibrated on the iPhone in step e) (scene.c). */
int bench_meshes(enum bench_api api, enum bench_load load);

/* The full-screen layers of a load: 0 for light and heavy, which keep their frames as before. */
int bench_fill_layers(enum bench_load load);

const char *bench_load_name(enum bench_load load);

/* The scene's time at `frame`, in seconds: the frame index at 120 frames per second. */
float bench_time(int frame);

/* The sphere: (S + 1) x (R + 1) vertices, 6 S R 16-bit indices, counter-clockwise from outside. */
enum { BENCH_SEGMENTS = 32, BENCH_RINGS = 12 };
enum { BENCH_VERTICES = (BENCH_SEGMENTS + 1) * (BENCH_RINGS + 1), BENCH_INDICES = 6 * BENCH_SEGMENTS * BENCH_RINGS };
struct bench_vertex {
    float position[3], normal[3], uv[2];
};
void bench_sphere(struct bench_vertex *vertices, uint16_t *indices);

/* The spheres' texture: RGBA8, 256 x 256 with its 9 levels, one after the other. */
enum { BENCH_TEXTURE_SIZE = 256, BENCH_TEXTURE_LEVELS = 9 };
size_t bench_texture_bytes(void);
void bench_texture(uint8_t *pixels);

/* Mesh `index`'s uniforms at `frame` on a window of `aspect` (width / height), column-major: the model's
   projection, its rotation as three columns padded to four floats, and its color. 128 bytes, Vulkan's push
   constants. */
struct bench_mesh {
    float mvp[16];
    float normal[12];
    float color[4];
};
void bench_mesh(int index, int frame, float aspect, struct bench_mesh *mesh);

/* The frames of a run, for scene.csv (index,tick_ns,cpu_ns). */
struct bench_frame {
    int64_t tick, cpu;
};
struct bench_frames {
    struct bench_frame *rows;
    int count, capacity;
};
int bench_frames_reserve(struct bench_frames *frames, int capacity);  // 0, or -1 without memory
int bench_frames_add(struct bench_frames *frames, int64_t tick, int64_t cpu);  // 0, or -1 without memory
int bench_frames_write(const struct bench_frames *frames, const char *path);  // 0, or -1
void bench_frames_free(struct bench_frames *frames);

/* The calling thread's CPU time in ns (CLOCK_THREAD_CPUTIME_ID). */
int64_t bench_cpu_ns(void);

/* CRC-32 (zlib's) of `bytes`, continued from `crc`, 0 to start. */
uint32_t bench_crc32(uint32_t crc, const uint8_t *bytes, size_t size);

/* A line of the platform's log (android/main.c, native/native.m). */
void bench_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
