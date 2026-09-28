/* The host build's EGL (ADR-0037, decision 1): ANGLE's libGLESv2 exports EGL_*, not egl*, and ANGLE's own libEGL
   forwards each egl* to its EGL_*; so do these names, which the scene's files use as EGL's. Included before
   EGL/egl.h, whose prototypes then declare the EGL_* names. */
#ifndef BENCH_ANGLE_EGL_H
#define BENCH_ANGLE_EGL_H

#define eglChooseConfig EGL_ChooseConfig
#define eglCreateContext EGL_CreateContext
#define eglCreateWindowSurface EGL_CreateWindowSurface
#define eglDestroyContext EGL_DestroyContext
#define eglDestroySurface EGL_DestroySurface
#define eglGetError EGL_GetError
#define eglGetPlatformDisplay EGL_GetPlatformDisplay
#define eglInitialize EGL_Initialize
#define eglMakeCurrent EGL_MakeCurrent
#define eglQuerySurface EGL_QuerySurface
#define eglReleaseThread EGL_ReleaseThread
#define eglSwapBuffers EGL_SwapBuffers
#define eglTerminate EGL_Terminate

#endif
