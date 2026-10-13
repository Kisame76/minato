// surface-texture-test's JNI library (ADR-0058 c), its third part): the NDK's ASurfaceTexture of the app's Java
// SurfaceTexture. Its window is a producer from native code, as a video decoder's or an engine's is; the consumer's
// calls run on the GL thread, in the context the Java side made current. Its answers hold on every device.
#include <android/native_window.h>
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JNI_NAME(name) Java_io_github_kisame76_minato_test_surfacetexture_SurfaceTextureApplication_##name

JNIEXPORT jlong JNICALL JNI_NAME(nativeFromSurfaceTexture)(JNIEnv *env, jclass clazz, jobject texture) {
    (void)clazz;
    return (jlong)(intptr_t)ASurfaceTexture_fromSurfaceTexture(env, texture);
}

// A frame through the texture's window, the top half `top` and the bottom half `bottom` (0xRRGGBBAA), in RGBA_8888.
JNIEXPORT jstring JNICALL JNI_NAME(nativeProduce)(JNIEnv *env, jclass clazz, jlong texture, jint top_color,
                                                  jint bottom_color) {
    (void)clazz;
    ANativeWindow *window = ASurfaceTexture_acquireANativeWindow((ASurfaceTexture *)(intptr_t)texture);
    ANativeWindow_Buffer buffer;
    memset(&buffer, 0, sizeof buffer);
    const int locked = ANativeWindow_lock(window, &buffer, NULL);
    for (int32_t y = 0; locked == 0 && y < buffer.height; y++) {
        for (int32_t x = 0; x < buffer.width; x++) {
            uint8_t *pixel = (uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4;
            const uint32_t color = (uint32_t)(y < buffer.height / 2 ? top_color : bottom_color);
            const uint8_t bytes[4] = {(uint8_t)(color >> 24), (uint8_t)(color >> 16), (uint8_t)(color >> 8),
                                      (uint8_t)color};
            memcpy(pixel, bytes, 4);
        }
    }
    const int posted = locked == 0 ? ANativeWindow_unlockAndPost(window) : locked;
    ANativeWindow_release(window);
    char text[160];
    snprintf(text, sizeof text, "lock %d, %d x %d in format %d, post %d", locked, buffer.width, buffer.height,
             buffer.format, posted);
    return (*env)->NewStringUTF(env, text);
}

// The consumer's calls on the GL thread: updateTexImage without a new frame, the timestamp and matrix, detach,
// updateTexImage while detached, attach to `name`.
JNIEXPORT jstring JNICALL JNI_NAME(nativeConsume)(JNIEnv *env, jclass clazz, jlong texture, jint name,
                                                  jlong timestamp) {
    (void)clazz;
    ASurfaceTexture *st = (ASurfaceTexture *)(intptr_t)texture;
    const int updated = ASurfaceTexture_updateTexImage(st);
    float matrix[16];
    ASurfaceTexture_getTransformMatrix(st, matrix);
    const int same_time = ASurfaceTexture_getTimestamp(st) == timestamp;
    const int detached = ASurfaceTexture_detachFromGLContext(st);
    const int while_detached = ASurfaceTexture_updateTexImage(st);
    const int attached = ASurfaceTexture_attachToGLContext(st, (uint32_t)name);
    char text[256];
    snprintf(text, sizeof text,
             "updateTexImage %d, the Java timestamp %d, matrix [%g %g %g %g], detach %d, updateTexImage detached %d, "
             "attach %d",
             updated, same_time, matrix[0], matrix[5], matrix[12], matrix[13], detached, while_detached, attached);
    return (*env)->NewStringUTF(env, text);
}

JNIEXPORT jint JNICALL JNI_NAME(nativeUpdate)(JNIEnv *env, jclass clazz, jlong texture) {
    (void)env, (void)clazz;
    return ASurfaceTexture_updateTexImage((ASurfaceTexture *)(intptr_t)texture);
}

JNIEXPORT void JNICALL JNI_NAME(nativeRelease)(JNIEnv *env, jclass clazz, jlong texture) {
    (void)env, (void)clazz;
    ASurfaceTexture_release((ASurfaceTexture *)(intptr_t)texture);
}
