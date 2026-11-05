package io.github.kisame76.minato.test.eglprobedecor;

import android.app.Activity;
import android.graphics.Canvas;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * The black-screen probe (ADR-0043 amendment / ADR-0058 c): an engine's start probe over a software surface, the
 * pattern SDL/Mono/Unity follow when they fill a surface by the CPU and then try a GL context on it at start and fall
 * back to software. A full-screen plain SurfaceView is filled once by the CPU (lockCanvas / unlockCanvasAndPost),
 * then an EGL context and a window surface are tried on the SAME surface and dropped, and the surface is never posted
 * again.
 *
 * With the connect-state fix (a window keeps one connected producer): the CPU producer holds the surface, so
 * eglCreateWindowSurface is refused (EGL_NO_SURFACE, EGL_BAD_ALLOC 0x3003), no GL drawable is presented, and the CPU
 * frame stays shown. Without it: the EGL window surface is made, its swap presents a drawable that hides the buffers'
 * sublayer, and the static CPU frame never reposts -- the window goes black. On the device MINATO_WINDOW_TRACE shows
 * which branch ran. The app draws once and logs the EGL outcome and "done"; hardwareAccelerated="false" so the decor
 * is software too, as the black apps are.
 */
public class EglProbeActivity extends Activity {
    private String TAG;
    private boolean mProbed;
    private SurfaceView mView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(TAG, "onCreate");
        mView = new SurfaceView(this);
        mView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                if (mProbed) {
                    return;
                }
                mProbed = true;
                // One opaque CPU frame, as a software UI draws it; this connects the CPU producer to the surface.
                final Canvas canvas = holder.lockCanvas();
                if (canvas != null) {
                    canvas.drawColor(0xFFCC3344);
                    holder.unlockCanvasAndPost(canvas);
                    Log.i(TAG, "cpu posted");
                } else {
                    Log.i(TAG, "cpu lock failed");
                }
                probeEgl(holder);
                // No further post: a static surface, so a drawable that hid it would leave it black.
                mView.post(() -> Log.i(TAG, "done"));
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {}
        });
        setContentView(mView);
    }

    /** An engine's GL probe on the surface the CPU already holds: it must be refused while the CPU is connected. */
    private void probeEgl(SurfaceHolder holder) {
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
        final EGLSurface eglSurface =
                EGL14.eglCreateWindowSurface(display, configs[0], holder.getSurface(), new int[] {EGL14.EGL_NONE}, 0);
        if (eglSurface == null || eglSurface.equals(EGL14.EGL_NO_SURFACE)) {
            // The fix: refused while the CPU producer holds the surface; the CPU frame stays shown.
            Log.i(TAG, "egl after cpu: refused, error 0x" + Integer.toHexString(EGL14.eglGetError()));
        } else {
            // main today: the surface is made, so the engine clears and swaps a frame; its drawable hides the CPU
            // buffer and the static surface goes black (the bug). Then the probe drops it, as an engine's fallback does.
            EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context);
            GLES20.glClearColor(0f, 1f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            EGL14.eglSwapBuffers(display, eglSurface);
            Log.i(TAG, "egl after cpu: a window surface was made and swapped (the pre-fix path)");
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(display, eglSurface);
        }
        if (context != null && !context.equals(EGL14.EGL_NO_CONTEXT)) {
            EGL14.eglDestroyContext(display, context);
        }
        EGL14.eglTerminate(display);
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(TAG, "focus " + hasFocus);
    }
}
