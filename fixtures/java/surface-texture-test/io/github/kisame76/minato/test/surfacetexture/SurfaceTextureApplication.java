package io.github.kisame76.minato.test.surfacetexture;

import android.app.Application;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Callable;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * ADR-0058 c)'s test of SurfaceTexture: AOSP's SurfaceTexture.java and Surface(SurfaceTexture) over Minato's buffer
 * queue. From Application.onCreate, which waits for each part on its own thread:
 * - a GL thread (a HandlerThread) makes an ES 2.0 context on a 16 x 16 pbuffer, an external texture and the
 *   SurfaceTexture of it, whose default buffer size is 8 x 8 and whose frame-available listener has no Handler, so
 *   it runs on the creator's Looper, the GL thread's;
 * - a producer thread locks the Surface's Canvas (its size), draws the top half red and the bottom half blue and posts
 *   it; on the GL thread the listener updates the texture, logs the transform matrix and that the timestamp is
 *   positive, and draws the texture through samplerExternalOES with the matrix applied to the texture coordinates; a
 *   pixel of the top half and one of the bottom half are read back;
 * - a second frame, all green, with a later timestamp; updateTexImage without a new frame keeps it; detach and attach
 *   to a new texture keep it too;
 * - GL as the producer (ADR-0058 c), its second part): after the Canvas's Surface is released, a window surface on a
 *   new Surface(SurfaceTexture), then one on the SurfaceTexture itself with a presentation time, which becomes the
 *   frame's timestamp; each draws the top half yellow and the bottom half cyan by scissored clears;
 * - the NDK's ASurfaceTexture (its third part, the JNI library): a frame through its window from native code, top
 *   magenta and bottom white, and the consumer's calls from native code on the GL thread; then, without a listener,
 *   a frame queued while detached, updateTexImage then, and after attaching again;
 * - three frames from the CPU without an updateTexImage between them: whether the producer waits, the onFrameAvailable
 *   calls, and which frame updateTexImage takes;
 * - release: isReleased, and what lockCanvas and updateTexImage answer afterwards.
 * The pixels are of one color each, exact in 8 bits. It logs with the tag surface-texture-test and ends with "done";
 * tools/android-reference.sh compares it with Android 15's log.
 */
public class SurfaceTextureApplication extends Application {
    private static final String TAG = "surface-texture-test";
    private static final int SIZE = 16;

    private static final String VERTEX = "attribute vec2 position;\n"
            + "uniform mat4 transform;\n"
            + "varying vec2 coordinate;\n"
            + "void main() {\n"
            + "  coordinate = (transform * vec4(position * 0.5 + 0.5, 0.0, 1.0)).xy;\n"
            + "  gl_Position = vec4(position, 0.0, 1.0);\n"
            + "}\n";
    private static final String FRAGMENT = "#extension GL_OES_EGL_image_external : require\n"
            + "precision mediump float;\n"
            + "uniform samplerExternalOES image;\n"
            + "varying vec2 coordinate;\n"
            + "void main() { gl_FragColor = texture2D(image, coordinate); }\n";

    private Handler gl;
    private Looper glLooper;
    private EGLDisplay display;
    private EGLSurface pbuffer;
    private EGLContext context;
    private int program;
    private SurfaceTexture texture;
    private Surface surface;
    private final float[] matrix = new float[16];
    private CountDownLatch frame;
    private long lastTimestamp;
    private volatile long presentationTime;

    @Override
    public void onCreate() {
        super.onCreate();
        HandlerThread thread = new HandlerThread("gl");
        thread.start();
        glLooper = thread.getLooper();
        gl = new Handler(glLooper);
        try {
            onGl(this::setUp);
            if (surface == null) {
                throw new IllegalStateException("no Surface of a SurfaceTexture");
            }
            produce("first frame", 0xffff0000, 0xff0000ff);
            produce("second frame", 0xff00ff00, 0xff00ff00);
            onGl(() -> {
                texture.updateTexImage();
                Log.i(TAG, "updateTexImage without a new frame: timestamp the same "
                        + (texture.getTimestamp() == lastTimestamp) + ", " + pixels());
                texture.detachFromGLContext();
                int[] names = new int[1];
                GLES20.glGenTextures(1, names, 0);
                texture.attachToGLContext(names[0]);
                Log.i(TAG, "attached to a new texture: " + pixels());
                return null;
            });
            // The Canvas's Surface stays connected as the CPU's producer until it goes; GL takes a new one.
            surface.release();
            Log.i(TAG, "the Canvas's Surface released: valid " + surface.isValid());
            surface = new Surface(texture);
            produceGl("third frame, GL through Surface(SurfaceTexture)", surface, false);
            produceGl("fourth frame, GL on the SurfaceTexture with a presentation time", texture, true);
            ndk();
            burst();
            onGl(() -> {
                texture.release();
                Log.i(TAG, "released: " + texture.isReleased());
                Log.i(TAG, "updateTexImage after release: " + outcome(() -> {
                    texture.updateTexImage();
                    return "updated";
                }));
                return null;
            });
            Log.i(TAG, "lockCanvas after release: " + outcome(() -> surface.lockCanvas(null)));
            surface.release();
            Log.i(TAG, "Surface released: valid " + surface.isValid());
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        thread.quitSafely();
        Log.i(TAG, "done");
    }

    private static String outcome(Callable<Object> call) {
        try {
            return String.valueOf(call.call());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /** Runs `part` on the GL thread and waits for it, at most 10 s; its exception goes to the log. */
    private void onGl(Callable<Object> part) throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        gl.post(() -> {
            try {
                part.call();
            } catch (Throwable t) {
                Log.i(TAG, "failed on the GL thread: " + t);
                Log.i(TAG + "-device", Log.getStackTraceString(t));
            }
            done.countDown();
        });
        if (!done.await(10, TimeUnit.SECONDS)) {
            Log.i(TAG, "the GL thread did not finish within 10 s");
        }
    }

    private Object setUp() {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        pbuffer = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL14.EGL_WIDTH, SIZE, EGL14.EGL_HEIGHT, SIZE, EGL14.EGL_NONE}, 0);
        context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context);
        program = program();
        int[] names = new int[1];
        GLES20.glGenTextures(1, names, 0);
        texture = new SurfaceTexture(names[0]);
        Log.i(TAG, "SurfaceTexture: released " + texture.isReleased());
        texture.setDefaultBufferSize(8, 8);
        texture.setOnFrameAvailableListener(available -> {
            available.updateTexImage();
            long timestamp = available.getTimestamp();
            if (timestamp == lastTimestamp) {
                return;  // a call without a new frame, which Android makes at times: the frame's own comes later
            }
            Log.i(TAG, "onFrameAvailable on the creator's Looper: " + (Looper.myLooper() == glLooper)
                    + ", the same SurfaceTexture " + (available == texture));
            available.getTransformMatrix(matrix);
            Log.i(TAG, "  transform " + Arrays.toString(matrix));
            Log.i(TAG, "  timestamp positive " + (timestamp > 0) + ", later than the last "
                    + (timestamp > lastTimestamp)
                    + (presentationTime != 0 ? ", the presentation time " + (timestamp == presentationTime) : ""));
            lastTimestamp = timestamp;
            Log.i(TAG, "  " + pixels());
            frame.countDown();
        });
        surface = new Surface(texture);
        Log.i(TAG, "Surface(SurfaceTexture): valid " + surface.isValid());
        return null;
    }

    /**
     * The NDK's ASurfaceTexture of the SurfaceTexture (its JNI library): a frame through its window, which the Java
     * listener takes, then the consumer's calls from native code on the GL thread, and its release.
     */
    private void ndk() throws InterruptedException {
        System.loadLibrary("surface-texture-test");
        long ndk = nativeFromSurfaceTexture(texture);
        Log.i(TAG, "ASurfaceTexture_fromSurfaceTexture: " + (ndk != 0));
        frame = new CountDownLatch(1);
        String produced = nativeProduce(ndk, 0xff00ffff, 0xffffffff);
        if (!frame.await(10, TimeUnit.SECONDS)) {
            Log.i(TAG, "ASurfaceTexture's window: no onFrameAvailable within 10 s");
        }
        Log.i(TAG, "ASurfaceTexture's window: " + produced);
        onGl(() -> {
            int[] names = new int[1];
            GLES20.glGenTextures(1, names, 0);
            Log.i(TAG, "ASurfaceTexture: " + nativeConsume(ndk, names[0], texture.getTimestamp()));
            Log.i(TAG, "  attached again: " + pixels());
            texture.setOnFrameAvailableListener(null);
            texture.detachFromGLContext();
            return null;
        });
        // A frame queued while the texture is detached and nobody listens.
        Log.i(TAG, "a frame while detached: " + nativeProduce(ndk, 0x00ff00ff, 0x0000ffff));
        Thread.sleep(200);
        onGl(() -> {
            Log.i(TAG, "  updateTexImage detached with a frame queued: " + nativeUpdate(ndk));
            int[] names = new int[1];
            GLES20.glGenTextures(1, names, 0);
            texture.attachToGLContext(names[0]);
            Log.i(TAG, "  attached: " + pixels());
            Log.i(TAG, "  updateTexImage attached: " + nativeUpdate(ndk) + ", " + pixels());
            return null;
        });
        nativeRelease(ndk);
        Log.i(TAG, "ASurfaceTexture_release: the SurfaceTexture released " + texture.isReleased());
    }

    /**
     * Three frames from the CPU without an updateTexImage between them, the producer given 3 s: whether it waits for
     * the consumer, how many onFrameAvailable calls come, and which frame updateTexImage takes, twice.
     */
    private void burst() throws InterruptedException {
        AtomicInteger calls = new AtomicInteger();
        texture.setOnFrameAvailableListener(available -> calls.incrementAndGet());
        Surface burst = new Surface(texture);
        Thread producer = new Thread(() -> {
            for (int color : new int[] {0xffff0000, 0xff00ff00, 0xff0000ff}) {
                Canvas canvas = burst.lockCanvas(null);
                canvas.drawColor(color);
                burst.unlockCanvasAndPost(canvas);
            }
        }, "burst");
        producer.setDaemon(true);
        producer.start();
        producer.join(3000);
        Log.i(TAG, "three frames without updateTexImage: the producer done " + !producer.isAlive());
        Thread.sleep(200);
        Log.i(TAG, "  onFrameAvailable calls " + calls.get());
        onGl(() -> {
            texture.updateTexImage();
            Log.i(TAG, "  updateTexImage: " + pixels());
            texture.updateTexImage();
            Log.i(TAG, "  updateTexImage again: " + pixels());
            return null;
        });
        burst.release();
    }

    private static native long nativeFromSurfaceTexture(SurfaceTexture texture);

    private static native String nativeProduce(long texture, int top, int bottom);

    private static native int nativeUpdate(long texture);

    private static native String nativeConsume(long texture, int name, long timestamp);

    private static native void nativeRelease(long texture);

    /**
     * A frame from GL on a producer thread, its window surface on `window` (a Surface or the SurfaceTexture): the top
     * half yellow and the bottom half cyan through two scissored clears, the scissor test left on; `timed`, with now as
     * its presentation time, later than the last frame's and earlier than the next one's. Waits for the listener.
     */
    private void produceGl(String name, Object window, boolean timed) throws InterruptedException {
        frame = new CountDownLatch(1);
        String[] swap = {name + ": no swap"};
        Thread producer = new Thread(() -> {
            int[] spec = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                    EGL14.EGL_WINDOW_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
            EGLContext own = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            EGLSurface windowSurface = EGL14.eglCreateWindowSurface(display, configs[0], window,
                    new int[] {EGL14.EGL_NONE}, 0);
            Log.i(TAG, name + ": eglCreateWindowSurface " + (windowSurface != EGL14.EGL_NO_SURFACE) + ", error 0x"
                    + Integer.toHexString(EGL14.eglGetError()));
            EGL14.eglMakeCurrent(display, windowSurface, windowSurface, own);
            int[] width = new int[1];
            int[] height = new int[1];
            EGL14.eglQuerySurface(display, windowSurface, EGL14.EGL_WIDTH, width, 0);
            EGL14.eglQuerySurface(display, windowSurface, EGL14.EGL_HEIGHT, height, 0);
            Log.i(TAG, name + ": surface " + width[0] + " x " + height[0]);
            GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
            GLES20.glScissor(0, height[0] / 2, width[0], height[0] - height[0] / 2);
            GLES20.glClearColor(1, 1, 0, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            GLES20.glScissor(0, 0, width[0], height[0] / 2);
            GLES20.glClearColor(0, 1, 1, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            if (timed) {
                long time = System.nanoTime();
                presentationTime = time;
                EGLExt.eglPresentationTimeANDROID(display, windowSurface, time);
            }
            // The listener may run before the swap returns, so its result is logged once the frame is in.
            swap[0] = name + ": eglSwapBuffers " + EGL14.eglSwapBuffers(display, windowSurface)
                    + ", the scissor test still on " + GLES20.glIsEnabled(GLES20.GL_SCISSOR_TEST);
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(display, windowSurface);
            EGL14.eglDestroyContext(display, own);
        }, "gl producer");
        producer.start();
        producer.join();
        if (!frame.await(10, TimeUnit.SECONDS)) {
            Log.i(TAG, name + ": no onFrameAvailable within 10 s");
        }
        Log.i(TAG, swap[0]);
        presentationTime = 0;
    }

    /** A frame from a producer thread: the top half `top`, the bottom half `bottom`; waits for its listener. */
    private void produce(String name, int top, int bottom) throws InterruptedException {
        frame = new CountDownLatch(1);
        Thread producer = new Thread(() -> {
            Canvas canvas = surface.lockCanvas(null);
            Log.i(TAG, name + ": Canvas " + canvas.getWidth() + " x " + canvas.getHeight());
            Paint paint = new Paint();
            paint.setColor(top);
            canvas.drawRect(0, 0, canvas.getWidth(), canvas.getHeight() / 2f, paint);
            paint.setColor(bottom);
            canvas.drawRect(0, canvas.getHeight() / 2f, canvas.getWidth(), canvas.getHeight(), paint);
            surface.unlockCanvasAndPost(canvas);
        }, "producer");
        producer.start();
        producer.join();
        if (!frame.await(10, TimeUnit.SECONDS)) {
            Log.i(TAG, name + ": no onFrameAvailable within 10 s");
        }
    }

    private static int shader(int type, String source) {
        int made = GLES20.glCreateShader(type);
        GLES20.glShaderSource(made, source);
        GLES20.glCompileShader(made);
        return made;
    }

    private static int program() {
        int made = GLES20.glCreateProgram();
        GLES20.glAttachShader(made, shader(GLES20.GL_VERTEX_SHADER, VERTEX));
        GLES20.glAttachShader(made, shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT));
        GLES20.glBindAttribLocation(made, 0, "position");
        GLES20.glLinkProgram(made);
        int[] linked = new int[1];
        GLES20.glGetProgramiv(made, GLES20.GL_LINK_STATUS, linked, 0);
        Log.i(TAG, "program linked " + (linked[0] == GLES20.GL_TRUE));
        return made;
    }

    /** Draws the texture with the matrix over the pbuffer and reads a pixel of its top and its bottom half. */
    private String pixels() {
        FloatBuffer quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        quad.put(new float[] {-1, -1, 1, -1, -1, 1, 1, 1}).position(0);
        GLES20.glViewport(0, 0, SIZE, SIZE);
        GLES20.glClearColor(0, 0, 0, 0);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(program);
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "transform"), 1, false, matrix, 0);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "image"), 0);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, quad);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        ByteBuffer top = ByteBuffer.allocateDirect(4);
        ByteBuffer bottom = ByteBuffer.allocateDirect(4);
        GLES20.glReadPixels(SIZE / 2, SIZE * 3 / 4, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, top);
        GLES20.glReadPixels(SIZE / 2, SIZE / 4, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, bottom);
        return "pixels: top " + hex(top) + ", bottom " + hex(bottom) + ", error 0x"
                + Integer.toHexString(GLES20.glGetError());
    }

    private static String hex(ByteBuffer pixel) {
        StringBuilder text = new StringBuilder();
        for (int index = 0; index < 4; index++) {
            text.append(String.format("%02x", pixel.get(index) & 0xff));
        }
        return text.toString();
    }
}
