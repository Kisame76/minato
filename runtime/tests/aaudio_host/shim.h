// Included before everything else when aaudio_library_test compiles runtime/android/aaudio.cpp for the Mac: what
// bionic's headers give the guest build and macOS lacks. The library's syscall() goes to minato_host_syscall
// (aaudio_library_test.cpp), which calls the runtime's dispatcher as the guest's syscall would.
#pragma once

#include <time.h>

#define __INTRODUCED_IN(api_level)

// The guest's CLOCK_MONOTONIC is Darwin's CLOCK_UPTIME_RAW (F10), the clock of the runtime's deadlines; CLOCK_BOOTTIME
// also counts sleep, as Darwin's CLOCK_MONOTONIC does.
#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_UPTIME_RAW
#define CLOCK_BOOTTIME _CLOCK_MONOTONIC
