// Ours for core/jni's GLES bindings (ADR-0041 a), Grafik), which framework/native/patches/opengl-without-libgui.patch
// points here in place of libgui's, libui's and libEGL's private headers: the window behind a Java Surface, and the
// Unwritten log line of the natives the patch leaves out.
#pragma once

#include <android/native_window.h>
#include <jni.h>

namespace android {

// The libnativewindow window of `surface`, acquired for the caller, who releases it with ANativeWindow_release; null
// when it has none. Until Grafik's Surface step gives Surface its natives no Surface has one: the first call logs that
// window surfaces wait for that step, and the bindings then throw their IllegalArgumentException.
ANativeWindow *minato_surface_window(JNIEnv *env, jobject surface);

// Logs `method` once through com.android.minato.services.Unwritten, as the generated managers do (ADR-0045 decision
// 3): "Unwritten: <method> is not implemented".
void minato_unwritten(JNIEnv *env, const char *method);

}  // namespace android
