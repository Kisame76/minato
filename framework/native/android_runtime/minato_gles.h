// Ours for core/jni's GLES bindings under ADR-0041 a), which framework/native/patches/opengl-without-libgui.patch
// points here in place of libgui's, libui's and libEGL's private headers: the window behind a Java Surface, and the
// Unwritten log line of the natives the patch leaves out.
#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <jni.h>

namespace android {

// The libnativewindow window of `surface`, acquired for the caller, who releases it with ANativeWindow_release; null
// for a released Surface or another object, and the bindings then throw their IllegalArgumentException. Defined with
// Surface's natives (android_view_Surface.cpp, ADR-0043 c2), which keep the window in mNativeObject.
ANativeWindow *minato_surface_window(JNIEnv *env, jobject surface);

// The queue window of the SurfaceTexture `surfaceTexture` (ADR-0058 c)), acquired for the caller, who releases it; null
// for a finalized SurfaceTexture. Defined with SurfaceTexture's natives (android_graphics_SurfaceTexture.cpp).
ANativeWindow *minato_surface_texture_window(JNIEnv *env, jobject surfaceTexture);

// For a TextureView in a window drawn in software (android_view_TextureView.cpp, ADR-0058 d)), in place of hwui's
// DeferredLayerUpdater: the newest queued frame of `surfaceTexture` becomes its current frame, as updateTexImage makes
// it, without a GL context. Returns the current frame's buffer, acquired for the caller, who releases it with
// AHardwareBuffer_release; null without a frame. `fresh` says whether a new frame came.
AHardwareBuffer *minato_surface_texture_latch(JNIEnv *env, jobject surfaceTexture, bool *fresh);

// Logs `method` once through com.android.minato.services.Unwritten, as the generated managers do (ADR-0045 decision
// 3): "Unwritten: <method> is not implemented".
void minato_unwritten(JNIEnv *env, const char *method);

}  // namespace android
