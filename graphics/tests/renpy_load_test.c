/* renpy-load-test: Ren'Py 8.5.3's texture loader on Android (renpy_load.h), the order graphics.gles_resume_renpy loads
   its set by, with Android 15's answers from the emulator, where it runs as a shell program: so the resume variant's
   order is one a GL answers as Android's does, not only one ANGLE takes. In a pbuffer's ES 3 context, at the start
   and again at a return (on_resize: quit_fbo, init_fbo, the set deleted and loaded again):
   - init_fbo: draw.fbo and its 1 x 1 twin complete, the FTL program linked;
   - three textures of one color each through load_gltexture: each kept texture's level 0 and its last level (1 x 1)
     read back (the draw into draw.fbo with blending, the copy of level 0, the mipmaps);
   - what the order leaves alive, by glIs*: the kept textures, no temporary (a temporary's name that a kept texture
     took again counts as the kept one's), no pixel unpack buffer, draw.fbo's two framebuffers and two renderbuffers,
     its program; after quit_fbo none of draw.fbo's, and its program, deleted while in use (Ren'Py does not unbind
     it), still a program until the next init_fbo's program is used (ES 3.0, 7.3);
   - the scene's draw of a kept texture to the window.
   Each step's GL error is printed; no value of the emulator alone (no version, no name numbers). */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <string.h>

#include "renpy_load.h"

enum { width = 64, height = 48, textures = 3 };
static const unsigned char colors[textures][4] = {{200, 40, 120, 255}, {10, 250, 60, 255}, {90, 90, 255, 255}};

static const char *yn(int value) { return value ? "yes" : "no"; }

/* The texture's level, read back through a framebuffer of its own, deleted at once. */
static void read_level(GLuint texture, GLint level, unsigned char pixel[4]) {
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
    memset(pixel, 0, 4);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
}

static int complete(GLuint framebuffer) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    const int done = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return done;
}

static void init_fbo(const char *when, struct renpy_draw *draw) {
    renpy_init_fbo(draw, width, height);
    GLint linked = GL_FALSE;
    glGetProgramiv(draw->ftl, GL_LINK_STATUS, &linked);
    const int fbo = complete(draw->fbo), fbo_1px = complete(draw->fbo_1px);
    printf("%s: init_fbo: draw.fbo complete: %s, its 1 x 1 twin complete: %s, the FTL program linked: %s, error 0x%x\n",
           when, yn(fbo), yn(fbo_1px), yn(linked == GL_TRUE), glGetError());
}

/* The set through load_gltexture, each kept texture read back, then what is alive. */
static void load(const char *when, const struct renpy_draw *draw, GLuint kept[textures], GLuint old_program) {
    static unsigned char pixels[renpy_side * renpy_side * 4];
    GLuint temporaries[textures], buffers[textures];
    for (int t = 0; t < textures; t++) {
        for (int p = 0; p < renpy_side * renpy_side; p++) {
            memcpy(&pixels[p * 4], colors[t], 4);
        }
        kept[t] = renpy_load_texture(draw, pixels, &temporaries[t], &buffers[t]);
        unsigned char first[4], last[4];
        read_level(kept[t], 0, first);
        read_level(kept[t], renpy_levels - 1, last);
        printf("%s: texture %d: level 0 reads %u %u %u %u, level %d (1 x 1) reads %u %u %u %u, error 0x%x\n", when,
               t + 1, first[0], first[1], first[2], first[3], renpy_levels - 1, last[0], last[1], last[2], last[3],
               glGetError());
    }
    renpy_load_end(width, height);
    int textures_alive = 0, temporaries_alive = 0, buffers_alive = 0;
    for (int t = 0; t < textures; t++) {
        textures_alive += glIsTexture(kept[t]) == GL_TRUE;
        int taken = 0;
        for (int k = 0; k < textures; k++) {
            taken |= temporaries[t] == kept[k];
        }
        temporaries_alive += !taken && glIsTexture(temporaries[t]) == GL_TRUE;
        buffers_alive += glIsBuffer(buffers[t]) == GL_TRUE;
    }
    const int framebuffers = (glIsFramebuffer(draw->fbo) == GL_TRUE) + (glIsFramebuffer(draw->fbo_1px) == GL_TRUE);
    const int renderbuffers =
        (glIsRenderbuffer(draw->color) == GL_TRUE) + (glIsRenderbuffer(draw->color_1px) == GL_TRUE);
    printf("%s: alive: kept textures %d of %d, temporaries %d, pixel unpack buffers %d, draw.fbo's framebuffers %d and "
           "renderbuffers %d, its program: %s", when, textures_alive, textures, temporaries_alive, buffers_alive,
           framebuffers, renderbuffers, yn(glIsProgram(draw->ftl) == GL_TRUE));
    if (old_program != 0) {
        printf(", the program quit_fbo deleted still a program: %s", yn(glIsProgram(old_program) == GL_TRUE));
    }
    printf(", error 0x%x\n", glGetError());
    /* The scene: the last texture over the window. */
    renpy_draw_ftl(draw, kept[textures - 1]);
    unsigned char pixel[4] = {0, 0, 0, 0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("%s: the scene's draw of texture %d to the window reads %u %u %u %u, error 0x%x\n", when, textures, pixel[0],
           pixel[1], pixel[2], pixel[3], glGetError());
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    const EGLBoolean initialized = eglInitialize(display, &major, &minor);
    const EGLint wanted[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    eglChooseConfig(display, wanted, &config, 1, &configs);
    const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const EGLSurface pbuffer = eglCreatePbufferSurface(display, config, size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLBoolean current = eglMakeCurrent(display, pbuffer, pbuffer, context);
    printf("ready: initialized: %s, a config: %s, current: %s, %s\n", yn(initialized == EGL_TRUE), yn(configs > 0),
           yn(current == EGL_TRUE), glGetError() == GL_NO_ERROR ? "GL_NO_ERROR" : "an error");

    struct renpy_draw draw;
    GLuint kept[textures];
    init_fbo("start", &draw);
    load("start", &draw, kept, 0);

    /* The return: on_resize(full_reset=True), then the loader's quit (the set deleted) and the scene loaded again. */
    renpy_quit_fbo(&draw);
    const GLuint old_program = draw.ftl;
    const int framebuffers = (glIsFramebuffer(draw.fbo) == GL_TRUE) + (glIsFramebuffer(draw.fbo_1px) == GL_TRUE);
    const int renderbuffers = (glIsRenderbuffer(draw.color) == GL_TRUE) + (glIsRenderbuffer(draw.color_1px) == GL_TRUE);
    printf("return: quit_fbo: draw.fbo's framebuffers alive %d, renderbuffers %d, its program deleted while in use "
           "still a program: %s, error 0x%x\n", framebuffers, renderbuffers, yn(glIsProgram(old_program) == GL_TRUE),
           glGetError());
    init_fbo("return", &draw);
    glDeleteTextures(textures, kept);
    int alive = 0;
    for (int t = 0; t < textures; t++) {
        alive += glIsTexture(kept[t]) == GL_TRUE;
    }
    printf("return: the set deleted: kept textures alive %d of %d, error 0x%x\n", alive, textures, glGetError());
    load("return", &draw, kept, old_program);

    glDeleteTextures(textures, kept);
    renpy_quit_fbo(&draw);
    glUseProgram(0);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    eglTerminate(display);
    printf("done\n");
    return 0;
}
