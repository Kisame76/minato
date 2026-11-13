// minato_activity_paused: the query the SDL swap override (libminato-sdl.so, runtime/android/sdl.c) asks, exported
// from libc.so so it is in the override's lookup scope (the override imports it weak). It answers 1 from the
// activity's onPause (APKRT_EVENT_PAUSE/STOP) until its onResume (APKRT_EVENT_RESUME), and 0 otherwise -- the window
// in which SDL 2 backs its context up and a flip that fails there is a dropped frame, not a lost display. On Android
// nothing asks it.
//
// The runtime (the host) owns the state: apkrt_session_post toggles a single int beside the stall watchdog's
// pause/resume, so this query and the watchdog always agree. The loader enters that int's address into
// minato_activity_paused_flag once, before any guest thread, at the SDL preload (runtime/src/dl.cpp
// preload_sdl_swap); until then it is null (a process with no SDL override) and the query answers 0, so the weak
// import just passes SDL's answer on. The read is relaxed: the host writes the int as an atomic (std::atomic<int>),
// and a read that races a toggle only misses one frame's worth of the window, which the override already treats as a
// dropped frame.
//
// stdatomic.h is a freestanding compiler header, so it resolves under bionic's -nostdlibinc; 0 stands in for NULL so
// the unit needs no bionic include at all.
#include <stdatomic.h>

int *minato_activity_paused_flag;

int minato_activity_paused(void) {
    int *flag = minato_activity_paused_flag;
    return flag != 0 ? atomic_load_explicit((_Atomic int *)flag, memory_order_relaxed) : 0;
}
