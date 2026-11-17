/* Ren'Py 8.5.3's texture loader on Android as GL calls (renpy/gl2/gl2draw.pyx init_fbo and quit_fbo,
   gl2texture.pyx load_gltexture, allocate_texture and mipmap_texture, the shader cache's renpy.ftl; GitHub tag
   8.5.3.26051504), for the set of graphics.gles_resume_renpy (gles_resume_test.c) and for renpy-load-test, which holds
   the same order against Android 15 on the emulator. The GLES renderer on Android is not draw.angle, so uploads take
   the pixel unpack buffer branch. Needs an ES 3 context current and a framebuffer of at least renpy_side x renpy_side.

   init_fbo makes draw.fbo with an RGBA8 renderbuffer of the window's size and a second of 1 x 1 (no depth, as
   config.depth_size is unset), and the shader cache's FTL program. Per texture load_gltexture makes a temporary texture
   (NEAREST, CLAMP_TO_EDGE) filled from a pixel unpack buffer that it then deletes, draws it with blending into draw.fbo
   by the FTL program from client-side vertex arrays, allocates the texture it keeps with every mip level
   (allocate_texture: one glTexImage2D each, no data), copies level 0 from draw.fbo with glCopyTexImage2D, mipmaps it
   (mipmap_texture) and deletes the temporary. on_resize's quit_fbo deletes draw.fbo, its renderbuffers and the program
   (the shader cache's clear), which the next init_fbo makes again. */
#ifndef MINATO_GRAPHICS_RENPY_LOAD_H
#define MINATO_GRAPHICS_RENPY_LOAD_H

#include <GLES3/gl3.h>

enum { renpy_side = 32, renpy_levels = 6 };  /* the textures are 32 x 32: levels 32 down to 1 */

/* draw.fbo and its 1 x 1 twin, their renderbuffers, and the FTL program. */
struct renpy_draw {
    GLuint fbo, fbo_1px, color, color_1px, ftl;
};

/* gl2draw's init_fbo and the shader cache's load of renpy.ftl, which draws a texture as it is. */
static inline void renpy_init_fbo(struct renpy_draw *draw, GLsizei width, GLsizei height) {
    glGenFramebuffers(1, &draw->fbo);
    glGenFramebuffers(1, &draw->fbo_1px);
    glGenRenderbuffers(1, &draw->color);
    glGenRenderbuffers(1, &draw->color_1px);
    glBindRenderbuffer(GL_RENDERBUFFER, draw->color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, draw->color_1px);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, draw->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, draw->color);
    glBindFramebuffer(GL_FRAMEBUFFER, draw->fbo_1px);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, draw->color_1px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    static const char *const vertex = "attribute vec4 a_position;\nattribute vec2 a_tex_coord;\n"
                                      "varying vec2 v_tex_coord;\nvoid main() {\n    v_tex_coord = a_tex_coord;\n"
                                      "    gl_Position = a_position;\n}\n";
    static const char *const fragment = "precision mediump float;\nuniform sampler2D tex0;\nvarying vec2 v_tex_coord;\n"
                                        "void main() {\n    gl_FragColor = texture2D(tex0, v_tex_coord);\n}\n";
    const GLuint shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
    glShaderSource(shaders[0], 1, &vertex, NULL);
    glShaderSource(shaders[1], 1, &fragment, NULL);
    draw->ftl = glCreateProgram();
    for (int i = 0; i < 2; i++) {
        glCompileShader(shaders[i]);
        glAttachShader(draw->ftl, shaders[i]);
    }
    glBindAttribLocation(draw->ftl, 0, "a_position");
    glBindAttribLocation(draw->ftl, 1, "a_tex_coord");
    glLinkProgram(draw->ftl);
    for (int i = 0; i < 2; i++) {
        glDeleteShader(shaders[i]);
    }
}

/* on_resize's quit_fbo and the shader cache's clear. The program stays in use (no glUseProgram(0)), as in Ren'Py. */
static inline void renpy_quit_fbo(const struct renpy_draw *draw) {
    glDeleteFramebuffers(1, &draw->fbo);
    glDeleteRenderbuffers(1, &draw->color);
    glDeleteFramebuffers(1, &draw->fbo_1px);
    glDeleteRenderbuffers(1, &draw->color_1px);
    glDeleteProgram(draw->ftl);
}

/* Program.draw_ftl: the texture as a quad over the bound framebuffer's viewport, from client-side vertex arrays. */
static inline void renpy_draw_ftl(const struct renpy_draw *draw, GLuint texture) {
    static const GLfloat position[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    static const GLfloat coordinate[] = {0, 0, 1, 0, 1, 1, 0, 1};
    glUseProgram(draw->ftl);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(draw->ftl, "tex0"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, position);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, coordinate);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* load_gltexture for renpy_side x renpy_side RGBA pixels: the texture Ren'Py keeps. The temporary's and the pixel
   unpack buffer's names, both deleted here, go to `temporary` and `pixel_buffer` when not NULL. draw.fbo stays bound
   with its viewport and blending: renpy_load_end gives the window back after the last texture. */
static inline GLuint renpy_load_texture(const struct renpy_draw *draw, const unsigned char *pixels, GLuint *temporary,
                                        GLuint *pixel_buffer) {
    GLuint texture = 0, premultiplied = 0, buffer = 0;
    glGenTextures(1, &texture);
    glGenTextures(1, &premultiplied);
    glBindFramebuffer(GL_FRAMEBUFFER, draw->fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)renpy_side * renpy_side * 4, pixels, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, renpy_side);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, renpy_side, renpy_side, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glDeleteBuffers(1, &buffer);  /* still bound, as Ren'Py leaves it: the delete unbinds it */
    glViewport(0, 0, renpy_side, renpy_side);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
    renpy_draw_ftl(draw, texture);
    /* allocate_texture: every level made without data; then level 0 copied from draw.fbo. */
    glBindTexture(GL_TEXTURE_2D, premultiplied);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, renpy_levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (GLint level = 0, side = renpy_side; level < renpy_levels; level++, side /= 2) {
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, renpy_side, renpy_side, 0);
    /* mipmap_texture */
    glBindTexture(GL_TEXTURE_2D, premultiplied);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, renpy_levels - 1);
    glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
    glGenerateMipmap(GL_TEXTURE_2D);
    glDeleteTextures(1, &texture);
    if (temporary != NULL) *temporary = texture;
    if (pixel_buffer != NULL) *pixel_buffer = buffer;
    return premultiplied;
}

/* After the last texture: blending off, the window's framebuffer and viewport again. */
static inline void renpy_load_end(GLsizei width, GLsizei height) {
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
}

#endif
