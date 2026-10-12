/* libEGL's EGL images of AHardwareBuffers (ADR-0058 b), decision 3): EGL_ANDROID_get_native_client_buffer and
   EGL_ANDROID_image_native_buffer.
   - eglGetNativeClientBufferANDROID gives the AHardwareBuffer itself as the client buffer, as no other function
     takes one; NULL is ANGLE's EGL_BAD_PARAMETER.
   - eglCreateImageKHR and eglCreateImage with EGL_NATIVE_BUFFER_ANDROID hand graphics/host the buffer's ID
     (AHardwareBuffer_getId), which it turns into an image of the buffer's IOSurface (graphics/host/image.m); a NULL
     buffer goes as the ID 0, which it answers with EGL_BAD_PARAMETER. Every other target goes to ANGLE as it is. */
#include <android/hardware_buffer.h>
#include <stdint.h>

#include "libegl.h"

MINATO_HOST(eglGetNativeClientBufferANDROID);
MINATO_HOST(eglCreateImageKHR);
MINATO_HOST(eglCreateImage);

EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *buffer) {
    // ANGLE checks the buffer for NULL and sets the error, EGL_SUCCESS for any other.
    (void)minato_host_eglGetNativeClientBufferANDROID(buffer);
    return (EGLClientBuffer)buffer;
}

/* The ID that graphics/host takes for a client buffer of eglGetNativeClientBufferANDROID's: 0 for none. */
static EGLClientBuffer id_of(EGLClientBuffer buffer) {
    uint64_t id = 0;
    if (buffer == NULL || AHardwareBuffer_getId == NULL ||
        AHardwareBuffer_getId((const AHardwareBuffer *)buffer, &id) != 0) {
        id = 0;
    }
    return (EGLClientBuffer)(uintptr_t)id;
}

EGLImageKHR eglCreateImageKHR(EGLDisplay display, EGLContext context, EGLenum target, EGLClientBuffer buffer,
                              const EGLint *attributes) {
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        buffer = id_of(buffer);
    }
    return minato_host_eglCreateImageKHR(display, context, target, buffer, attributes);
}

EGLImage eglCreateImage(EGLDisplay display, EGLContext context, EGLenum target, EGLClientBuffer buffer,
                        const EGLAttrib *attributes) {
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        buffer = id_of(buffer);
    }
    return minato_host_eglCreateImage(display, context, target, buffer, attributes);
}
