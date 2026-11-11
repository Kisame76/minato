package io.github.kisame76.minato.test.surfaceviewdecor;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.view.SurfaceHolder;

/** The gl variant: the SurfaceView's producer makes a GL context, clears green and swaps one frame, kept connected. */
public class GlProducerActivity extends SurfaceDecorActivity {
    @Override
    protected void produce(SurfaceHolder holder) {
        final EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        final int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        final int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE};
        final EGLConfig[] configs = new EGLConfig[1];
        final int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        final EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        // The SurfaceView's surface has no prior producer, so GL is its sole producer and the window surface is made.
        final EGLSurface surface =
                EGL14.eglCreateWindowSurface(display, configs[0], holder.getSurface(), new int[] {EGL14.EGL_NONE}, 0);
        if (surface == null || surface.equals(EGL14.EGL_NO_SURFACE)) {
            say("gl surface refused, error 0x" + Integer.toHexString(EGL14.eglGetError()));
            return;
        }
        EGL14.eglMakeCurrent(display, surface, surface, context);
        GLES20.glClearColor(0.2f, 0.8f, 0.27f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        EGL14.eglSwapBuffers(display, surface);
        say("gl surface posted");
    }
}
