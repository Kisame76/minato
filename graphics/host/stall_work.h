/* graphics/host's work sources for the runtime's stall watchdog (stall_work.c), from ANGLE's memory counters. */
#ifndef MINATO_STALL_WORK_H
#define MINATO_STALL_WORK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ANGLE's ANGLEMetalMemoryCounters (patch 0015), as angle.m found it in the libGLESv2 it opened; NULL without it. */
void minato_graphics_stall_counters(size_t (*counters)(uint64_t *values, size_t count));

/* An EGL window surface on host window `window` (angle.m, at each one ANGLE made): its texture and buffer bytes count
   as that window's work. The first one registers both sources with the runtime, once ANGLE's counters are known. */
void minato_graphics_stall_gl_window(uint64_t window);

/* The sources (apkrt_stall_set_work_source, apkrt_stall_set_return_work_source): see stall_work.c. */
uint64_t minato_graphics_stall_work(uint64_t id);
uint64_t minato_graphics_stall_return_work(uint64_t id);

#ifdef __cplusplus
}
#endif

#endif
