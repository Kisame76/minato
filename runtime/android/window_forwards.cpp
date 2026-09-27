// libandroid.so's ANativeWindow functions (ADR-0028, decision 7): forwards to libnativewindow.so, which
// holds the window. Each calls libnativewindow's function of the same name through its version
// LIBNATIVEWINDOW, since a plain reference would bind to libandroid's own.
#include <android/native_window.h>

#define FORWARD(result, name, parameters, arguments)                                    \
    extern "C" result minato_forward_##name parameters;                                \
    __asm__(".symver minato_forward_" #name ", " #name "@LIBNATIVEWINDOW");             \
    extern "C" result name parameters { return minato_forward_##name arguments; }

FORWARD(void, ANativeWindow_acquire, (ANativeWindow* window), (window))
FORWARD(void, ANativeWindow_release, (ANativeWindow* window), (window))
FORWARD(int32_t, ANativeWindow_getWidth, (ANativeWindow* window), (window))
FORWARD(int32_t, ANativeWindow_getHeight, (ANativeWindow* window), (window))
FORWARD(int32_t, ANativeWindow_getFormat, (ANativeWindow* window), (window))
FORWARD(int32_t, ANativeWindow_setBuffersGeometry, (ANativeWindow* window, int32_t width, int32_t height, int32_t format),
        (window, width, height, format))
FORWARD(int32_t, ANativeWindow_lock, (ANativeWindow* window, ANativeWindow_Buffer* buffer, ARect* dirty),
        (window, buffer, dirty))
FORWARD(int32_t, ANativeWindow_unlockAndPost, (ANativeWindow* window), (window))
