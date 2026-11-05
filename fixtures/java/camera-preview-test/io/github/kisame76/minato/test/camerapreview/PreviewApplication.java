package io.github.kisame76.minato.test.camerapreview;

import android.app.Application;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * ADR-0073 step e's test of the legacy android.hardware.Camera's preview, with CAMERA granted. Android 15's emulator's
 * camera is not Minato's, so the main tag holds only the answers every Android 15 camera gives (each check as true or
 * false, the exceptions by class and message) and the counts go to the device tag. From a thread that
 * Application.attachBaseContext starts, on camera 0 at the smallest preview size, its callbacks on a HandlerThread's
 * Looper:
 * - previewEnabled before and after startPreview into a SurfaceTexture whose GL thread takes each frame
 *   (updateTexImage), frames that come at least 10 in 2 s with rising timestamps;
 * - setPreviewCallback's frames: at least 10 in 2 s, each NV21's width x height x 3 / 2 bytes;
 * - setPreviewCallbackWithBuffer with three buffers: the frames come in those buffers and stop once all three are
 *   given out; setOneShotPreviewCallback: one frame;
 * - setParameters with another preview size while the preview runs, which fails;
 * - setPreviewTexture(null) while the preview runs, as a SurfaceView's surfaceDestroyed sets no display without a
 *   stopPreview: previewEnabled, no more frames into the first SurfaceTexture; then a second SurfaceTexture, whose
 *   frames come;
 * - stopPreview: previewEnabled, and no more frames.
 * Each wait is bounded; it logs with the tag camera-preview-test and ends with "done".
 */
public class PreviewApplication extends Application {
    private static final String TAG = "camera-preview-test";

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        new Thread(() -> {
            try {
                test();
            } catch (Throwable t) {
                add("failed: " + t);
                add("device " + Log.getStackTraceString(t));
            }
            add("done");
            flush();
        }, TAG).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    /** The lines, those that start with "device " to the device tag, a pause after every 10 (liblog drops lines). */
    private void flush() {
        int count = 0;
        for (String line : lines) {
            boolean device = line.startsWith("device ");
            Log.i(device ? TAG + "-device" : TAG, device ? line.substring(7) : line);
            if (++count % 10 == 0) pause(5);
        }
        lines.clear();
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** A GL thread with a pbuffer context and an external texture, whose SurfaceTexture takes each frame. */
    private static final class GlTarget {
        final HandlerThread thread = new HandlerThread("gl");
        Handler handler;
        SurfaceTexture texture;
        final AtomicInteger frames = new AtomicInteger();
        final AtomicLong lastTimestamp = new AtomicLong(-1);
        volatile boolean rising = true;
        EGLDisplay display;
        EGLContext context;
        EGLSurface surface;

        void start() throws InterruptedException {
            thread.start();
            handler = new Handler(thread.getLooper());
            CountDownLatch made = new CountDownLatch(1);
            handler.post(() -> {
                display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] version = new int[2];
                EGL14.eglInitialize(display, version, 0, version, 1);
                int[] attributes = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                    EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_NONE};
                EGLConfig[] configs = new EGLConfig[1];
                int[] count = new int[1];
                EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0);
                context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                        new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                        new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
                EGL14.eglMakeCurrent(display, surface, surface, context);
                int[] name = new int[1];
                GLES20.glGenTextures(1, name, 0);
                GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, name[0]);
                texture = new SurfaceTexture(name[0]);
                texture.setOnFrameAvailableListener(st -> {
                    st.updateTexImage();
                    long time = st.getTimestamp();
                    if (time <= lastTimestamp.get()) rising = false;
                    lastTimestamp.set(time);
                    frames.incrementAndGet();
                }, handler);
                made.countDown();
            });
            made.await(5, TimeUnit.SECONDS);
        }

        void end() throws InterruptedException {
            CountDownLatch done = new CountDownLatch(1);
            handler.post(() -> {
                texture.release();
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                EGL14.eglDestroySurface(display, surface);
                EGL14.eglDestroyContext(display, context);
                done.countDown();
            });
            done.await(5, TimeUnit.SECONDS);
            thread.quitSafely();
        }
    }

    private void test() throws Exception {
        HandlerThread callbacks = new HandlerThread("camera");
        callbacks.start();
        Camera[] opened = new Camera[1];
        CountDownLatch ready = new CountDownLatch(1);
        new Handler(callbacks.getLooper()).post(() -> {
            opened[0] = Camera.open(0);  // its callbacks then come on this Looper
            ready.countDown();
        });
        ready.await(10, TimeUnit.SECONDS);
        Camera camera = opened[0];
        Camera.Parameters parameters = camera.getParameters();
        Camera.Size smallest = null;
        for (Camera.Size size : parameters.getSupportedPreviewSizes()) {
            if (smallest == null || size.width * size.height < smallest.width * smallest.height) smallest = size;
        }
        parameters.setPreviewSize(smallest.width, smallest.height);
        camera.setParameters(parameters);
        final int frameBytes = smallest.width * smallest.height * 3 / 2;
        add("device preview size " + smallest.width + "x" + smallest.height);

        GlTarget target = new GlTarget();
        target.start();
        camera.setPreviewTexture(target.texture);
        add("preview enabled before the start " + camera.previewEnabled());
        camera.startPreview();
        add("preview enabled after the start " + camera.previewEnabled());
        Thread.sleep(2000);
        int frames = target.frames.get();
        add("frames into the SurfaceTexture: at least 10 in 2 s " + (frames >= 10) + ", their timestamps rising "
                + target.rising);
        add("device frames into the SurfaceTexture in 2 s: " + frames);

        AtomicInteger callbacksCount = new AtomicInteger();
        AtomicInteger wrongSize = new AtomicInteger();
        camera.setPreviewCallback((data, c) -> {
            callbacksCount.incrementAndGet();
            if (data == null || data.length != frameBytes) wrongSize.incrementAndGet();
        });
        Thread.sleep(2000);
        camera.setPreviewCallback(null);
        add("preview callbacks: at least 10 in 2 s " + (callbacksCount.get() >= 10) + ", each NV21's size "
                + (wrongSize.get() == 0));
        add("device preview callbacks in 2 s: " + callbacksCount.get());

        List<byte[]> buffers = new ArrayList<>();
        AtomicInteger withBuffer = new AtomicInteger();
        AtomicInteger foreign = new AtomicInteger();
        for (int n = 0; n < 3; n++) {
            byte[] buffer = new byte[frameBytes];
            buffers.add(buffer);
            camera.addCallbackBuffer(buffer);
        }
        camera.setPreviewCallbackWithBuffer((data, c) -> {
            withBuffer.incrementAndGet();
            boolean ours = false;
            for (byte[] buffer : buffers) ours |= buffer == data;
            if (!ours) foreign.incrementAndGet();
        });
        Thread.sleep(1500);
        camera.setPreviewCallbackWithBuffer(null);
        add("callbacks with buffers: three " + (withBuffer.get() == 3) + ", in the app's buffers "
                + (foreign.get() == 0));
        add("device callbacks with three buffers in 1.5 s: " + withBuffer.get());

        AtomicInteger oneShot = new AtomicInteger();
        camera.setOneShotPreviewCallback((data, c) -> oneShot.incrementAndGet());
        Thread.sleep(1000);
        add("one-shot callback: one " + (oneShot.get() == 1));

        Camera.Parameters other = camera.getParameters();
        Camera.Size larger = null;
        for (Camera.Size size : other.getSupportedPreviewSizes()) {
            if (!size.equals(smallest)) larger = size;
        }
        if (larger != null) {
            other.setPreviewSize(larger.width, larger.height);
            try {
                camera.setParameters(other);
                add("another preview size while previewing: no exception");
            } catch (RuntimeException e) {
                add("another preview size while previewing: " + e.getClass().getName() + " " + e.getMessage());
            }
        }

        try {
            camera.setPreviewTexture(null);
            add("no preview texture while previewing: no exception");
        } catch (IOException | RuntimeException e) {
            add("no preview texture while previewing: " + e.getClass().getName() + " " + e.getMessage());
        }
        add("preview enabled without a texture " + camera.previewEnabled());
        Thread.sleep(300);
        int detached = target.frames.get();
        Thread.sleep(700);
        add("frames into the first SurfaceTexture after it: none " + (target.frames.get() == detached));
        GlTarget second = new GlTarget();
        second.start();
        camera.setPreviewTexture(second.texture);
        add("preview enabled with a second texture " + camera.previewEnabled());
        Thread.sleep(2000);
        add("frames into the second SurfaceTexture: at least 10 in 2 s " + (second.frames.get() >= 10));
        add("device frames into the second SurfaceTexture in 2 s: " + second.frames.get());

        camera.stopPreview();
        add("preview enabled after the stop " + camera.previewEnabled());
        Thread.sleep(300);
        int stopped = second.frames.get();
        Thread.sleep(700);
        add("frames after the stop: none " + (second.frames.get() == stopped));
        camera.release();
        target.end();
        second.end();
        callbacks.quitSafely();
    }
}
