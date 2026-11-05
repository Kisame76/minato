package io.github.kisame76.minato.test.surfaceviewproducers;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * A SurfaceView's producers across its surface's changes, against Android 15: on a full-screen SurfaceView, from a
 * thread of its own, with the holder's callbacks logged as they come (a new surface is "created"):
 * - a) EGL's window surface on the fresh Surface, made current, cleared, swapped and destroyed while current, then
 *   one made again;
 * - b) a software frame (lockCanvas, unlockCanvasAndPost), then EGL's window surface;
 * - c) setFormat(RGBA_8888) after it, then EGL;
 * - d) another software frame, setFixedSize to half, then EGL;
 * - e) another software frame, the view hidden and shown, then EGL.
 * Every EGL line says whether a surface was made and eglGetError. Ends with "done".
 */
public class ProducersActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "surfaceview-producers-test";

    private final BlockingQueue<String> events = new LinkedBlockingQueue<>();
    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceView view;
    private EGLDisplay display;
    private EGLConfig config;
    private EGLContext context;
    private boolean started;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        events.add("created");
        if (!started) {
            started = true;
            new Thread(this::run, "producers").start();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        events.add("changed");
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        events.add("destroyed");
    }

    private void run() {
        try {
            steps();
        } catch (Throwable error) {
            Log.i(TAG, "failed: " + error);
        }
        Log.i(TAG, "done");
    }

    private void steps() throws InterruptedException {
        settle("start");
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        config = configs[0];
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);

        EGLSurface first = make("a) fresh");
        if (first != EGL14.EGL_NO_SURFACE) {
            boolean current = EGL14.eglMakeCurrent(display, first, first, context);
            GLES20.glClearColor(0f, 1f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            boolean swapped = EGL14.eglSwapBuffers(display, first);
            boolean destroyed = EGL14.eglDestroySurface(display, first);
            Log.i(TAG, "a) current " + current + ", swapped " + swapped + ", destroyed while current " + destroyed);
        }
        EGLSurface again = make("a) again after the destroy while current");
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        drop(again);

        software("b)");
        drop(make("b) after the software frame"));

        main.post(() -> view.getHolder().setFormat(PixelFormat.RGBA_8888));
        settle("c) setFormat");
        drop(make("c) after setFormat"));

        software("d)");
        main.post(() -> view.getHolder().setFixedSize(view.getWidth() / 2, view.getHeight() / 2));
        settle("d) setFixedSize");
        drop(make("d) after setFixedSize"));

        software("e)");
        main.post(() -> view.setVisibility(View.GONE));
        settle("e) hidden");
        main.post(() -> view.setVisibility(View.VISIBLE));
        settle("e) shown");
        drop(make("e) after hide and show"));
    }

    /** The callbacks that come within a second, as one line. */
    private void settle(String what) throws InterruptedException {
        StringBuilder seen = new StringBuilder();
        String event;
        while ((event = events.poll(1, TimeUnit.SECONDS)) != null) {
            seen.append(seen.length() == 0 ? "" : ", ").append(event);
        }
        Log.i(TAG, what + ": " + (seen.length() == 0 ? "no callback" : seen));
    }

    private void software(String what) throws InterruptedException {
        CountDownLatch posted = new CountDownLatch(1);
        final boolean[] locked = new boolean[1];
        main.post(() -> {
            Canvas canvas = view.getHolder().lockCanvas();
            if (canvas != null) {
                locked[0] = true;
                canvas.drawColor(Color.RED);
                view.getHolder().unlockCanvasAndPost(canvas);
            }
            posted.countDown();
        });
        posted.await(5, TimeUnit.SECONDS);
        Log.i(TAG, what + " software frame: locked " + locked[0]);
    }

    private EGLSurface make(String what) {
        Surface surface = view.getHolder().getSurface();
        EGLSurface made = surface != null && surface.isValid()
                ? EGL14.eglCreateWindowSurface(display, config, surface, new int[] {EGL14.EGL_NONE}, 0)
                : EGL14.EGL_NO_SURFACE;
        Log.i(TAG, what + ": a window surface " + (made != EGL14.EGL_NO_SURFACE) + ", eglGetError 0x"
                + Integer.toHexString(EGL14.eglGetError()) + ", surface valid "
                + (surface != null && surface.isValid()));
        return made;
    }

    private void drop(EGLSurface surface) {
        if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
    }
}
