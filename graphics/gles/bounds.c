/* libGLESv1_CM's 7 functions of Android's own (ADR-0041 a1), which the NDK exports and no registry defines, so
   tools/frontgen keeps them out of the table (the description's "outside"). Each hands its pointer to its plain
   function, the stub in this lib, and drops the count, as Android's opengl/libs/GLES_CM/gl.cpp does at the tag.
   frameworks/base's GLES10, GLES11 and GLImpl natives call them for java.nio buffers. */
#define GL_GLEXT_PROTOTYPES
#include <GLES/gl.h>
#include <GLES/glext.h>

GL_API void GL_APIENTRY glColorPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                             GLsizei count) {
    (void)count;
    glColorPointer(size, type, stride, pointer);
}

GL_API void GL_APIENTRY glNormalPointerBounds(GLenum type, GLsizei stride, const GLvoid *pointer, GLsizei count) {
    (void)count;
    glNormalPointer(type, stride, pointer);
}

GL_API void GL_APIENTRY glTexCoordPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                GLsizei count) {
    (void)count;
    glTexCoordPointer(size, type, stride, pointer);
}

GL_API void GL_APIENTRY glVertexPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                              GLsizei count) {
    (void)count;
    glVertexPointer(size, type, stride, pointer);
}

GL_API void GL_APIENTRY glPointSizePointerOESBounds(GLenum type, GLsizei stride, const GLvoid *pointer,
                                                    GLsizei count) {
    (void)count;
    glPointSizePointerOES(type, stride, pointer);
}

GL_API void GL_APIENTRY glMatrixIndexPointerOESBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                      GLsizei count) {
    (void)count;
    glMatrixIndexPointerOES(size, type, stride, pointer);
}

GL_API void GL_APIENTRY glWeightPointerOESBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                 GLsizei count) {
    (void)count;
    glWeightPointerOES(size, type, stride, pointer);
}
