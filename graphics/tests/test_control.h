/* The tests' way from a guest program to the host's side of graphics-run (ADR-0029, B1): the front lib
   "test", whose one host function test_control.m provides, plays the app for tests 6 and 7. Guest programs
   call minato_test (test_control_guest.c), which reaches it through the runtime's entry, as host code on the
   calling thread. Plain C, for both sides. */
#ifndef MINATO_TEST_CONTROL_H
#define MINATO_TEST_CONTROL_H

#include <stdint.h>

/* The table's length, slot 0 included, and hash, which both sides check. */
#define MINATO_TEST_TABLE_LENGTH 2u
#define MINATO_TEST_TABLE_HASH 0x6d696e61746f5431ull

enum minato_test_command {
    MINATO_TEST_WINDOW = 1,     /* the window window_host.m added, its ID, or 0 */
    MINATO_TEST_GATE,           /* a: 1 opens the window's gate, 0 closes it; apkrt_window_set_gate's result */
    MINATO_TEST_REMOVE,         /* the window is gone: apkrt_window_remove's result */
    MINATO_TEST_PRESENTS,       /* the window's presents so far */
    MINATO_TEST_BACKGROUND,     /* a: 1 into the background, 0 back to the foreground */
    MINATO_TEST_SCHEDULED,      /* "scheduled" with a ms to go, asked on a thread of the host's own: its answer */
    MINATO_TEST_COUNT_COMMITS,  /* counts the command buffers guest threads commit from now on, from 0 */
    MINATO_TEST_COMMITS,        /* those counted so far */
    MINATO_TEST_DRAWABLE_SIZE,  /* the window's layer's drawableSize: width << 32 | height, 0 without a layer */
    MINATO_TEST_HOST_DEBUG,     /* a, b, c: an EGL display, surface and context, which a thread of the host's
                                   own makes current in ANGLE to insert the debug message "on a host thread"
                                   (GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, ID 7) and releases;
                                   1 once done */
    MINATO_TEST_EVENT,          /* a new MTLSharedEvent of the system's GPU at 0, kept for the process's life:
                                   its address, for EGL_ANGLE_metal_shared_event_sync */
    MINATO_TEST_EVENT_SIGNAL,   /* a: such an event, b: the value it takes */
    MINATO_TEST_VSYNC_GRID,     /* a, b: a tick of the display link at a with the period b, in nanoseconds of the
                                   uptime clock (apkrt_vsync_post): the refresh grid graphics/host puts presentation
                                   times on (ADR-0041 decision 11) */
    MINATO_TEST_PRESENTED_AT,   /* a: 1 watches the presents from now on and answers 0; a: 0 answers how the Metal
                                   hook (count mode) saw the first drawable presented since: its presentAtTime: time
                                   in nanoseconds of the uptime clock, UINT64_MAX for one presented without a time
                                   (present), 0 before one */
};

#ifndef __APPLE__
/* For guest programs: the command's answer; 0 without graphics-run's provider. */
uint64_t minato_test(uint64_t command, uint64_t a, uint64_t b, uint64_t c);
#endif

#endif
