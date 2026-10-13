package io.github.kisame76.minato.test.imagereader;

import android.app.Application;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0058 e1)'s test of ImageReader (the graphics breadth's G3a), in Application.onCreate:
 * - an RGBA_8888 reader of 64 x 48 with maxImages 2: its getters, nothing to acquire, two Canvas frames at once
 *   through its surface (the second replaces the first, as Android 15 drops a frame an app's producer queues over one
 *   not acquired yet), then frames one by one, two held at once with their planes, pixels, timestamps, crop and
 *   hardware buffer, the third acquire refused while two are held, a closed image refusing its planes,
 *   acquireLatestImage over three frames, and the reader's close;
 * - an RGB_565 reader fed by a Canvas, its plane of 2-byte pixels;
 * - an RGBA_8888 reader fed by GL through an EGL window surface on its surface, with eglPresentationTimeANDROID;
 * - a PRIVATE reader fed by GL: no planes, a hardware buffer.
 * Each wait for a frame is bounded; the log ends with "done".
 */
public class ImageReaderApplication extends Application {
    private static final String TAG = "image-reader-test";

    private HandlerThread mThread;
    private Handler mHandler;
    private final Semaphore mFrames = new Semaphore(0);
    private volatile boolean mOnHandlerThread = true;

    @Override
    public void onCreate() {
        super.onCreate();
        mThread = new HandlerThread("image-reader-test");
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        try {
            rgba();
            rgb565();
            gl(ImageFormat.UNKNOWN);
            gl(ImageFormat.PRIVATE);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        mThread.quitSafely();
        Log.i(TAG, "done");
    }

    private ImageReader reader(int width, int height, int format, int maxImages) {
        ImageReader reader = ImageReader.newInstance(width, height, format, maxImages);
        reader.setOnImageAvailableListener(r -> {
            if (Looper.myLooper() != mThread.getLooper()) {
                mOnHandlerThread = false;
            }
            mFrames.release();
        }, mHandler);
        return reader;
    }

    /** Waits for `count` onImageAvailable calls, at most two seconds; how many came. */
    private int await(int count) throws InterruptedException {
        int came = 0;
        while (came < count && mFrames.tryAcquire(2, TimeUnit.SECONDS)) {
            came++;
        }
        return came;
    }

    private static void draw(Surface surface, int color) {
        Canvas canvas = surface.lockCanvas(null);
        canvas.drawColor(color);
        surface.unlockCanvasAndPost(canvas);
    }

    /** The pixel's bytes at (x, y) of the image's first plane, in hex. */
    private static String pixel(Image image, int x, int y) {
        Image.Plane plane = image.getPlanes()[0];
        ByteBuffer buffer = plane.getBuffer();
        int at = y * plane.getRowStride() + x * plane.getPixelStride();
        StringBuilder hex = new StringBuilder();
        for (int i = 0; i < plane.getPixelStride(); i++) {
            hex.append(String.format("%02x", buffer.get(at + i)));
        }
        return hex.toString();
    }

    /** An acquire's outcome: the image's first pixel, which it closes, null or the refusal. */
    private static String attempt(ImageReader reader) {
        try {
            Image image = reader.acquireNextImage();
            if (image == null) {
                return "null";
            }
            String got = "pixel " + pixel(image, 0, 0);
            image.close();
            return got;
        } catch (IllegalStateException e) {
            return e.getMessage();
        }
    }

    /**
     * The planes: their count and pixel stride, and whether the row stride and the buffer fit the image; the row
     * stride and the buffer's size, which the buffers' allocator chooses (gralloc's on Android, Metal's alignment on
     * Minato), go to the device tag.
     */
    private static String planes(Image image) {
        Image.Plane[] planes = image.getPlanes();
        int pixelStride = planes[0].getPixelStride();
        int rowStride = planes[0].getRowStride();
        int capacity = planes[0].getBuffer().capacity();
        int row = image.getWidth() * pixelStride;
        Log.i(TAG + "-device", image.getWidth() + " x " + image.getHeight() + ": row stride " + rowStride + ", buffer "
                + capacity + " bytes");
        return planes.length + " plane, pixel stride " + pixelStride + ", row stride fits " + (rowStride >= row)
                + ", buffer covers the rows " + (capacity >= rowStride * (image.getHeight() - 1) + row);
    }

    private void rgba() throws Exception {
        ImageReader reader = reader(64, 48, PixelFormat.RGBA_8888, 2);
        Surface surface = reader.getSurface();
        Log.i(TAG, "RGBA reader: " + reader.getWidth() + " x " + reader.getHeight() + ", format "
                + reader.getImageFormat() + ", maxImages " + reader.getMaxImages() + ", usage " + reader.getUsage()
                + ", hardware buffer format " + reader.getHardwareBufferFormat() + ", data space "
                + reader.getDataSpace() + ", surface valid " + surface.isValid());
        Log.i(TAG, "nothing queued: acquireNextImage " + reader.acquireNextImage() + ", acquireLatestImage "
                + reader.acquireLatestImage());

        draw(surface, 0xffff0000);
        draw(surface, 0xff00ff00);
        int calls = await(2);
        Image kept = reader.acquireNextImage();
        Log.i(TAG, "two frames at once: onImageAvailable " + calls + " times, on the handler's thread "
                + mOnHandlerThread + ", acquired pixel (0, 0) " + pixel(kept, 0, 0) + ", then "
                + reader.acquireNextImage());
        kept.close();

        draw(surface, 0xffff0000);
        await(1);
        Image first = reader.acquireNextImage();
        draw(surface, 0xff00ff00);
        await(1);
        Image second = reader.acquireNextImage();
        Log.i(TAG, "first: format " + first.getFormat() + ", " + first.getWidth() + " x " + first.getHeight() + ", "
                + planes(first) + ", pixel (10, 10) " + pixel(first, 10, 10) + ", crop " + first.getCropRect()
                + ", data space " + first.getDataSpace() + ", timestamp set " + (first.getTimestamp() > 0));
        Log.i(TAG, "second, while the first is held: pixel (63, 47) " + pixel(second, 63, 47) + ", timestamp later "
                + (second.getTimestamp() > first.getTimestamp()));
        HardwareBuffer buffer = second.getHardwareBuffer();
        Log.i(TAG, "second's hardware buffer: " + buffer.getWidth() + " x " + buffer.getHeight() + ", format "
                + buffer.getFormat() + ", layers " + buffer.getLayers() + ", CPU read "
                + ((buffer.getUsage() & HardwareBuffer.USAGE_CPU_READ_OFTEN) != 0));
        buffer.close();

        draw(surface, 0xff0000ff);
        await(1);
        String third;
        try {
            third = "acquired " + reader.acquireNextImage();
        } catch (IllegalStateException e) {
            third = e.getMessage();
        }
        Log.i(TAG, "a third while two are held: " + third);
        first.close();
        first.close();
        String planes;
        try {
            planes = "planes " + first.getPlanes().length;
        } catch (IllegalStateException e) {
            planes = e.getMessage();
        }
        Log.i(TAG, "first closed: " + planes);
        Log.i(TAG, "after the first's close: " + attempt(reader));
        second.close();
        Log.i(TAG, "after the second's close: " + attempt(reader));
        Log.i(TAG, "then: " + attempt(reader));

        draw(surface, 0xffffffff);
        draw(surface, 0xff808080);
        draw(surface, 0xff000000);
        await(3);
        Image latest = reader.acquireLatestImage();
        Log.i(TAG, "acquireLatestImage of three: pixel " + pixel(latest, 1, 1) + ", then acquireNextImage "
                + reader.acquireNextImage());
        latest.close();

        reader.close();
        Log.i(TAG, "closed reader: surface valid " + surface.isValid() + ", acquireNextImage "
                + reader.acquireNextImage());
    }

    private void rgb565() throws Exception {
        ImageReader reader = reader(32, 16, PixelFormat.RGB_565, 1);
        draw(reader.getSurface(), 0xff3366cc);
        await(1);
        Image image = reader.acquireNextImage();
        Log.i(TAG, "RGB_565: format " + image.getFormat() + ", " + planes(image) + ", pixel (5, 5) "
                + pixel(image, 5, 5));
        image.close();
        reader.close();
    }

    /** A GL frame through an EGL window surface on the reader's surface: RGBA_8888's pixels or PRIVATE's buffer. */
    private void gl(int kind) throws Exception {
        boolean opaque = kind == ImageFormat.PRIVATE;
        ImageReader reader = reader(64, 48, opaque ? ImageFormat.PRIVATE : PixelFormat.RGBA_8888, 1);
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] attributes = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0);
        EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface window = EGL14.eglCreateWindowSurface(display, configs[0], reader.getSurface(),
                new int[] {EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(display, window, window, context);
        GLES20.glClearColor(0.2f, 0.4f, 0.6f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        EGLExt.eglPresentationTimeANDROID(display, window, 123456789L);
        boolean swapped = EGL14.eglSwapBuffers(display, window);
        int came = await(1);
        Image image = reader.acquireNextImage();
        String what;
        if (opaque) {
            String planes;
            try {
                planes = "planes " + image.getPlanes().length;
            } catch (UnsupportedOperationException e) {
                planes = e.getMessage();
            }
            HardwareBuffer buffer = image.getHardwareBuffer();
            what = "PRIVATE: format " + image.getFormat() + ", " + planes + ", hardware buffer " + buffer.getWidth()
                    + " x " + buffer.getHeight() + ", GPU sampled "
                    + ((buffer.getUsage() & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0);
            buffer.close();
        } else {
            what = "GL: " + planes(image) + ", pixel (5, 5) " + pixel(image, 5, 5);
        }
        Log.i(TAG, what + ", swapped " + swapped + ", onImageAvailable " + came + ", timestamp "
                + image.getTimestamp());
        image.close();
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(display, window);
        EGL14.eglDestroyContext(display, context);
        reader.close();
    }
}
