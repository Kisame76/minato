/* ADR-0065 step 4, on ANGLE's macOS slice with graphics-run's window (graphics.gles_profile and
   graphics.gles_profile_pixel): what glGetString answers for GL_VENDOR and GL_RENDERER. Without a device profile they
   are ANGLE's own (printed as "ANGLE's", as their text depends on the Mac); under one, the properties
   ro.minato.gpu.gl_vendor and ro.minato.gpu.gl_renderer, which the runtime writes, are what libGLESv2 answers, and
   nothing else changes: GL_VERSION still begins with "OpenGL ES 3.0", GL_EXTENSIONS is the list libGLESv2 keeps
   (non-empty, one name per glGetStringi index) and GL_SHADING_LANGUAGE_VERSION is ANGLE's. */
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <stdio.h>
#include <string.h>

static const char *named(const GLubyte *string, const char *angle) {
    if (string == NULL) {
        return "(null)";
    }
    return strstr((const char *)string, angle) != NULL ? "ANGLE's" : (const char *)string;
}

int main(void) {
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    eglInitialize(display, &major, &minor);
    const EGLint wanted[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    eglChooseConfig(display, wanted, &config, 1, &configs);
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLSurface pbuffer = eglCreatePbufferSurface(display, config, size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    if (!eglMakeCurrent(display, pbuffer, pbuffer, context)) {
        printf("no context: 0x%x\n", eglGetError());
        return 1;
    }
    printf("GL_VENDOR: %s\n", named(glGetString(GL_VENDOR), "Google Inc."));
    printf("GL_RENDERER: %s\n", named(glGetString(GL_RENDERER), "ANGLE ("));
    const GLubyte *gl_version = glGetString(GL_VERSION);
    printf("GL_VERSION begins with OpenGL ES 3.0: %s\n",
           gl_version != NULL && strncmp((const char *)gl_version, "OpenGL ES 3.0", 13) == 0 ? "yes" : "no");
    const GLubyte *language = glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GL_SHADING_LANGUAGE_VERSION begins with OpenGL ES GLSL ES 3.00: %s\n",
           language != NULL && strncmp((const char *)language, "OpenGL ES GLSL ES 3.00", 22) == 0 ? "yes" : "no");
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    const GLubyte *first = count > 0 ? glGetStringi(GL_EXTENSIONS, 0) : NULL;
    printf("GL_NUM_EXTENSIONS is above 0 and glGetStringi names the first: %s\n",
           count > 0 && first != NULL ? "yes" : "no");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    eglTerminate(display);
    return 0;
}
