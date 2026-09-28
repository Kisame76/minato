/* ANGLE's Metal display and window surfaces as libEGL asks for them (ADR-0029, decisions 14 and 15), shared with the
   parity benchmark's native build (ADR-0037, decision 1), which asks ANGLE for the same. ANGLE's names, which the
   NDK's headers lack (ANGLE's include/EGL/eglext_angle.h and eglext.h). */
#ifndef MINATO_METAL_DISPLAY_H
#define MINATO_METAL_DISPLAY_H

#define MINATO_EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define MINATO_EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define MINATO_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define MINATO_EGL_FIXED_SIZE_ANGLE 0x3201

/* eglGetPlatformDisplay's attributes for ANGLE's Metal display, with EGL_PLATFORM_ANGLE_ANGLE and
   EGL_DEFAULT_DISPLAY. */
#define MINATO_EGL_METAL_DISPLAY \
    {MINATO_EGL_PLATFORM_ANGLE_TYPE_ANGLE, MINATO_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE}

/* A window surface's size, fixed at the window's (EGL_ANGLE_window_fixed_size, patch 0001). */
#define MINATO_EGL_FIXED_SIZE(width, height) \
    MINATO_EGL_FIXED_SIZE_ANGLE, EGL_TRUE, EGL_WIDTH, (width), EGL_HEIGHT, (height)

#endif
