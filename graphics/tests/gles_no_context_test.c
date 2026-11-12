/* What libGLESv2 answers on a thread without a current context, against Android 15, whose wrapper sends every call
   to its no-context hooks then: nothing is done, nothing is written, and the answer is 0 (a null pointer, GL_FALSE,
   GL_NO_ERROR). First on a thread that never had a context, then on one whose context was released
   (eglMakeCurrent with EGL_NO_CONTEXT), as a game's render thread is after SDL's pause. Each value that a call
   would write is preset to 0x7e57 (or the text "unchanged"), so that a write shows. */
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void answers(const char *when) {
    printf("%s:\n", when);
    printf("  glGetError: 0x%x\n", glGetError());
    printf("  glGetString(GL_VERSION): %s\n", glGetString(GL_VERSION) == NULL ? "null" : "a string");
    printf("  glGetString(GL_EXTENSIONS): %s\n", glGetString(GL_EXTENSIONS) == NULL ? "null" : "a string");
    printf("  glGetStringi(GL_EXTENSIONS, 0): %s\n", glGetStringi(GL_EXTENSIONS, 0) == NULL ? "null" : "a string");
    printf("  glCreateProgram: %u\n", glCreateProgram());
    printf("  glCreateShader: %u\n", glCreateShader(GL_VERTEX_SHADER));
    GLuint name = 0x7e57;
    glGenTextures(1, &name);
    printf("  glGenTextures writes: %s\n", name == 0x7e57 ? "nothing" : "a name");
    name = 0x7e57;
    glGenBuffers(1, &name);
    printf("  glGenBuffers writes: %s\n", name == 0x7e57 ? "nothing" : "a name");
    name = 0x7e57;
    glGenFramebuffers(1, &name);
    printf("  glGenFramebuffers writes: %s\n", name == 0x7e57 ? "nothing" : "a name");
    GLint value = 0x7e57;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    printf("  glGetIntegerv writes: %s\n", value == 0x7e57 ? "nothing" : "a value");
    value = 0x7e57;
    glGetShaderiv(1, GL_COMPILE_STATUS, &value);
    printf("  glGetShaderiv writes: %s\n", value == 0x7e57 ? "nothing" : "a value");
    value = 0x7e57;
    glGetProgramiv(1, GL_LINK_STATUS, &value);
    printf("  glGetProgramiv writes: %s\n", value == 0x7e57 ? "nothing" : "a value");
    char log[16] = "unchanged";
    GLsizei length = 0x7e57;
    glGetShaderInfoLog(1, sizeof log, &length, log);
    const int untouched = length == 0x7e57 && strcmp(log, "unchanged") == 0;
    printf("  glGetShaderInfoLog writes: %s\n", untouched ? "nothing" : "a log");
    printf("  glGetUniformLocation: %d\n", glGetUniformLocation(1, "x"));
    printf("  glGetAttribLocation: %d\n", glGetAttribLocation(1, "x"));
    printf("  glGetUniformBlockIndex: 0x%x\n", glGetUniformBlockIndex(1, "x"));
    printf("  glCheckFramebufferStatus: 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    printf("  glIsTexture: %d\n", glIsTexture(1));
    printf("  glIsEnabled: %d\n", glIsEnabled(GL_BLEND));
    printf("  glMapBufferRange: %s\n",
           glMapBufferRange(GL_ARRAY_BUFFER, 0, 4, GL_MAP_READ_BIT) == NULL ? "null" : "a pointer");
    printf("  glUnmapBuffer: %d\n", glUnmapBuffer(GL_ARRAY_BUFFER));
    printf("  glFenceSync: %s\n", glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) == NULL ? "null" : "a sync");
    printf("  glClientWaitSync: 0x%x\n", glClientWaitSync((GLsync)(uintptr_t)1, 0, 0));
    printf("  glGetGraphicsResetStatus: 0x%x\n", glGetGraphicsResetStatus());
    printf("  glGetError at the end: 0x%x\n", glGetError());
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  // every line out before a crash
    answers("before any context");
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
    printf("made current: %s\n", eglMakeCurrent(display, pbuffer, pbuffer, context) ? "yes" : "no");
    glGetError();
    printf("released: %s\n", eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? "yes" : "no");
    answers("after the context's release");
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    eglTerminate(display);
    printf("done\n");
    return 0;
}
