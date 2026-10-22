package io.github.kisame76.minato.test.imagewriter;

import android.app.Application;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.media.ImageWriter;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * ADR-0058 e1)'s test of ImageWriter (the graphics breadth's G3b), in Application.onCreate:
 * - a writer on an RGBA_8888 reader: its getters and the Builder's, a dequeued image's format, size, planes,
 *   timestamp, crop and data space, a crop off the origin refused by the reader, which holds one image fewer from
 *   then on, the image the app wrote reaching the reader with its timestamp and crop, onImageReleased after the
 *   reader's close, a queued image refusing its planes, maxImages' refusal of a dequeue, a dequeued image's close
 *   (a cancel), three frames queued unread (kept or dropped);
 * - the writer's dequeues while the reader holds its images: how many come before one waits, and whether that one
 *   returns after the reader's close (each wait bounded by a timeout, so the test never hangs);
 * - a reader's image queued into a writer on a second reader (detach and attach);
 * - a writer on a SurfaceTexture: onFrameAvailable;
 * - a PRIVATE writer: its format, and dequeueInputImage's refusal;
 * - a writer on a YUV_420_888 reader (ADR-0060 e2a): the dequeued and the acquired image's three planes, a pattern
 *   written through them and read back, and Surface.lockCanvas on the reader's surface. Strides and the order of Cb
 *   and Cr are the allocator's, so only what Android promises is logged; the strides go to the device tag.
 * Each wait is bounded; the log ends with "done".
 */
public class ImageWriterApplication extends Application {
    private static final String TAG = "image-writer-test";

    private HandlerThread mThread;
    private Handler mHandler;
    private final Semaphore mFrames = new Semaphore(0);
    private final Semaphore mReleased = new Semaphore(0);

    @Override
    public void onCreate() {
        super.onCreate();
        mThread = new HandlerThread("image-writer-test");
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        try {
            writer();
            waits();
            attach();
            surfaceTexture();
            opaque();
            yuv();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        mThread.quitSafely();
        Log.i(TAG, "done");
    }

    private ImageReader reader(int width, int height, int format, int maxImages) {
        ImageReader reader = ImageReader.newInstance(width, height, format, maxImages);
        reader.setOnImageAvailableListener(r -> mFrames.release(), mHandler);
        return reader;
    }

    private void listen(ImageWriter writer) {
        writer.setOnImageReleasedListener(w -> mReleased.release(), mHandler);
    }

    /** Waits for `count` permits of `semaphore`, at most two seconds each; how many came. */
    private static int await(Semaphore semaphore, int count) throws InterruptedException {
        int came = 0;
        while (came < count && semaphore.tryAcquire(2, TimeUnit.SECONDS)) {
            came++;
        }
        return came;
    }

    /** Fills the image's first plane with one RGBA color. */
    private static void fill(Image image, int r, int g, int b, int a) {
        Image.Plane plane = image.getPlanes()[0];
        ByteBuffer buffer = plane.getBuffer();
        for (int y = 0; y < image.getHeight(); y++) {
            for (int x = 0; x < image.getWidth(); x++) {
                int at = y * plane.getRowStride() + x * plane.getPixelStride();
                buffer.put(at, (byte) r);
                buffer.put(at + 1, (byte) g);
                buffer.put(at + 2, (byte) b);
                buffer.put(at + 3, (byte) a);
            }
        }
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

    /** An acquire's outcome: the image's pixel (0, 0), which it closes, null or the refusal. */
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
     * A dequeue on another thread, at most two seconds: the image; a dequeue that waits longer throws, and its thread
     * stays blocked on the writer until its queue is abandoned, so the test never hangs.
     */
    private static Image take(ImageWriter writer) throws InterruptedException {
        AtomicReference<Object> result = new AtomicReference<>();
        Thread thread = dequeue(writer, result);
        thread.join(2000);
        Object got = result.get();
        if (got == null) {
            throw new IllegalStateException("the dequeue waits");
        }
        if (got instanceof RuntimeException) {
            throw (RuntimeException) got;
        }
        return (Image) got;
    }

    /** A dequeue, written in one color and queued. */
    private static void send(ImageWriter writer, int r, int g, int b) throws InterruptedException {
        Image image = take(writer);
        fill(image, r, g, b, 0xff);
        writer.queueInputImage(image);
    }

    private static String describe(ImageWriter writer) {
        return "maxImages " + writer.getMaxImages() + ", " + writer.getWidth() + " x " + writer.getHeight()
                + ", format " + writer.getFormat() + ", hardware buffer format " + writer.getHardwareBufferFormat()
                + ", data space " + writer.getDataSpace() + ", usage " + writer.getUsage();
    }

    private void writer() throws Exception {
        ImageReader reader = reader(64, 48, PixelFormat.RGBA_8888, 2);
        ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 2);
        listen(writer);
        Log.i(TAG, "writer on an RGBA reader: " + describe(writer));
        String second;
        try {
            ImageWriter.newInstance(reader.getSurface(), 1).close();
            second = "made";
        } catch (RuntimeException e) {
            second = e.getClass().getSimpleName() + " " + e.getMessage();
        }
        Log.i(TAG, "a second writer on the same surface: " + second);
        ImageReader other = ImageReader.newInstance(8, 8, PixelFormat.RGBA_8888, 1);
        ImageWriter built = new ImageWriter.Builder(other.getSurface()).setMaxImages(3)
                .setWidthAndHeight(32, 16).setHardwareBufferFormat(HardwareBuffer.RGBA_8888)
                .setUsage(HardwareBuffer.USAGE_CPU_WRITE_OFTEN).build();
        Log.i(TAG, "built: " + describe(built));
        built.close();
        other.close();

        Image image = take(writer);
        Image.Plane[] planes = image.getPlanes();
        Log.i(TAG, "dequeued: format " + image.getFormat() + ", " + image.getWidth() + " x " + image.getHeight()
                + ", " + planes.length + " plane, pixel stride " + planes[0].getPixelStride() + ", row stride fits "
                + (planes[0].getRowStride() >= image.getWidth() * 4) + ", timestamp " + image.getTimestamp()
                + ", crop " + image.getCropRect() + ", data space " + image.getDataSpace());
        Log.i(TAG + "-device", "dequeued row stride " + planes[0].getRowStride());
        fill(image, 0xff, 0, 0, 0xff);
        image.setTimestamp(12345L);
        image.setCropRect(new Rect(1, 2, 30, 40));
        writer.queueInputImage(image);
        String queued;
        try {
            queued = "planes " + image.getPlanes().length;
        } catch (IllegalStateException e) {
            queued = e.getMessage();
        }
        Log.i(TAG, "queued image: " + queued);
        int frames = await(mFrames, 1);
        String refused;
        try {
            Image bad = reader.acquireNextImage();
            refused = "acquired, crop " + bad.getCropRect();
            bad.close();
        } catch (UnsupportedOperationException e) {
            refused = e.getMessage();
        }
        Log.i(TAG, "crop off the origin: onImageAvailable " + frames + ", acquire: " + refused);

        image = take(writer);
        fill(image, 0, 0xff, 0, 0xff);
        image.setTimestamp(12345L);
        image.setCropRect(new Rect(0, 0, 30, 40));
        writer.queueInputImage(image);
        frames = await(mFrames, 1);
        Image got = reader.acquireNextImage();
        Log.i(TAG, "reader: onImageAvailable " + frames + ", pixel (5, 5) " + pixel(got, 5, 5) + ", timestamp "
                + got.getTimestamp() + ", crop " + got.getCropRect() + ", released before the reader's close "
                + mReleased.tryAcquire(200, TimeUnit.MILLISECONDS));
        // The refused frame's item is lost, as Android's: with maxImages 2, the image held is all the reader allows.
        send(writer, 0, 0, 0xff);
        await(mFrames, 1);
        String more;
        try {
            Image extra = reader.acquireNextImage();
            more = "acquired " + (extra != null);
            if (extra != null) {
                extra.close();
            }
        } catch (IllegalStateException e) {
            more = e.getMessage();
        }
        Log.i(TAG, "a second image next to it, with the refused frame's item lost: " + more);
        got.close();
        Log.i(TAG, "onImageReleased after the reader's close: " + await(mReleased, 1) + ", then " + attempt(reader)
                + ", onImageReleased " + await(mReleased, 1));

        Image a = take(writer);
        Image b = take(writer);
        String third;
        try {
            third = "dequeued " + writer.dequeueInputImage();
        } catch (IllegalStateException e) {
            third = e.getMessage();
        }
        Log.i(TAG, "a third dequeue while two are out: " + third);
        a.close();
        Image c = take(writer);
        Log.i(TAG, "after a dequeued image's close: dequeued " + (c != null) + ", reader " + attempt(reader)
                + ", onImageReleased " + mReleased.tryAcquire(200, TimeUnit.MILLISECONDS));
        b.close();
        c.close();

        mFrames.drainPermits();
        mReleased.drainPermits();
        send(writer, 0xff, 0, 0);
        send(writer, 0, 0xff, 0);
        send(writer, 0, 0, 0xff);
        int calls = await(mFrames, 3);
        Log.i(TAG, "three queued unread: onImageAvailable " + calls + ", then " + attempt(reader) + ", "
                + attempt(reader) + ", " + attempt(reader) + ", " + attempt(reader) + "; onImageReleased "
                + await(mReleased, 3));
        writer.close();
        String closed;
        try {
            closed = "dequeued " + writer.dequeueInputImage();
        } catch (IllegalStateException e) {
            closed = e.getMessage();
        }
        Log.i(TAG, "closed writer: " + closed);
        reader.close();
    }

    /** A dequeue on another thread, bounded: the image, or "waits" after a second. */
    private static Thread dequeue(ImageWriter writer, AtomicReference<Object> result) {
        Thread thread = new Thread(() -> {
            try {
                result.set(writer.dequeueInputImage());
            } catch (Throwable t) {
                result.set(t);
            }
        });
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static String outcome(Object result) {
        if (result == null) {
            return "waits";
        }
        if (result instanceof Throwable) {
            return result.getClass().getSimpleName() + " " + ((Throwable) result).getMessage();
        }
        return "an image";
    }

    private void waits() throws Exception {
        ImageReader reader = reader(16, 16, PixelFormat.RGBA_8888, 1);
        ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 1);
        mFrames.drainPermits();
        send(writer, 0xff, 0, 0);
        await(mFrames, 1);
        Image held = reader.acquireNextImage();
        int sent = 0;
        String last = null;
        while (sent < 8) {
            AtomicReference<Object> result = new AtomicReference<>();
            Thread thread = dequeue(writer, result);
            thread.join(1000);
            last = outcome(result.get());
            if (!"an image".equals(last)) {
                Log.i(TAG, "while the reader holds one: " + sent + " more frames went, then the dequeue " + last);
                held.close();
                thread.join(2000);
                Log.i(TAG, "after the reader's close: " + outcome(result.get()));
                if (result.get() instanceof Image) {
                    ((Image) result.get()).close();
                }
                break;
            }
            Image image = (Image) result.get();
            fill(image, 0, 0xff, 0, 0xff);
            writer.queueInputImage(image);
            sent++;
        }
        if (sent == 8) {
            Log.i(TAG, "while the reader holds one: 8 more frames went");
            held.close();
        }
        writer.close();
        reader.close();
    }

    private void attach() throws Exception {
        ImageReader source = reader(32, 32, PixelFormat.RGBA_8888, 2);
        ImageReader target = reader(32, 32, PixelFormat.RGBA_8888, 2);
        ImageWriter writer = ImageWriter.newInstance(target.getSurface(), 2);
        listen(writer);
        mFrames.drainPermits();
        mReleased.drainPermits();
        Surface surface = source.getSurface();
        Canvas canvas = surface.lockCanvas(null);
        canvas.drawColor(0xff00ffff);
        surface.unlockCanvasAndPost(canvas);
        await(mFrames, 1);
        Image image = source.acquireNextImage();
        image.setTimestamp(777L);
        writer.queueInputImage(image);
        int frames = await(mFrames, 1);
        String planes;
        try {
            planes = "planes " + image.getPlanes().length;
        } catch (IllegalStateException e) {
            planes = e.getMessage();
        }
        Image moved = target.acquireNextImage();
        Log.i(TAG, "a reader's image queued into a writer: the source image " + planes + ", target frames " + frames
                + ", pixel " + pixel(moved, 3, 3) + ", timestamp " + moved.getTimestamp());
        moved.close();
        Log.i(TAG, "onImageReleased after the target's close: " + await(mReleased, 1) + ", source then "
                + attempt(source));
        writer.close();
        source.close();
        target.close();
    }

    private void surfaceTexture() throws Exception {
        SurfaceTexture texture = new SurfaceTexture(false);
        texture.setDefaultBufferSize(8, 4);
        Semaphore available = new Semaphore(0);
        texture.setOnFrameAvailableListener(t -> available.release(), mHandler);
        Surface surface = new Surface(texture);
        ImageWriter writer = ImageWriter.newInstance(surface, 1, PixelFormat.RGBA_8888);
        Log.i(TAG, "writer on a SurfaceTexture: " + writer.getWidth() + " x " + writer.getHeight() + ", format "
                + writer.getFormat());
        send(writer, 0xff, 0xff, 0);
        Log.i(TAG, "onFrameAvailable: " + await(available, 1));
        writer.close();
        surface.release();
        texture.release();
    }

    private void opaque() throws Exception {
        ImageReader reader = reader(16, 16, ImageFormat.PRIVATE, 1);
        ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 1);
        Log.i(TAG, "PRIVATE writer: " + describe(writer));
        String dequeued;
        try {
            Image image = take(writer);
            dequeued = "an image of format " + image.getFormat() + ", " + image.getWidth() + " x "
                    + image.getHeight();
            try {
                dequeued += ", planes " + image.getPlanes().length;
            } catch (RuntimeException e) {
                dequeued += ", planes: " + e.getClass().getSimpleName() + " " + e.getMessage();
            }
            image.close();
        } catch (RuntimeException e) {
            dequeued = e.getClass().getSimpleName() + " " + e.getMessage();
        }
        Log.i(TAG, "PRIVATE dequeue: " + dequeued);
        writer.close();
        reader.close();
    }

    /** What Android promises of a YUV_420_888 image's planes; their strides to the device tag. */
    private static String yuvPlanes(Image image) {
        Image.Plane[] planes = image.getPlanes();
        if (planes.length != 3) {
            return planes.length + " planes";
        }
        Image.Plane y = planes[0], cb = planes[1], cr = planes[2];
        Log.i(TAG + "-device", "strides: Y " + y.getPixelStride() + " " + y.getRowStride() + ", Cb "
                + cb.getPixelStride() + " " + cb.getRowStride() + ", Cr " + cr.getPixelStride() + " "
                + cr.getRowStride());
        int width = image.getWidth(), height = image.getHeight();
        boolean chroma = cb.getPixelStride() == cr.getPixelStride()
                && (cb.getPixelStride() == 1 || cb.getPixelStride() == 2) && cb.getRowStride() == cr.getRowStride()
                && cb.getRowStride() >= width / 2 * cb.getPixelStride();
        boolean sizes = y.getBuffer().remaining() >= (height - 1) * y.getRowStride() + width
                && cb.getBuffer().remaining() >= (height / 2 - 1) * cb.getRowStride()
                        + (width / 2 - 1) * cb.getPixelStride() + 1
                && cr.getBuffer().remaining() >= (height / 2 - 1) * cr.getRowStride()
                        + (width / 2 - 1) * cr.getPixelStride() + 1;
        return "3 planes, Y's pixel stride " + y.getPixelStride() + ", Y's rows at least the width "
                + (y.getRowStride() >= width) + ", Cb and Cr alike " + chroma + ", buffers that hold their samples "
                + sizes;
    }

    /** Writes (or, with check, compares) a pattern through a YUV_420_888 image's planes; whether all came back. */
    private static boolean yuvPattern(Image image, boolean check) {
        Image.Plane[] planes = image.getPlanes();
        boolean same = true;
        for (int p = 0; p < 3; p++) {
            int rows = p == 0 ? image.getHeight() : image.getHeight() / 2;
            int columns = p == 0 ? image.getWidth() : image.getWidth() / 2;
            ByteBuffer buffer = planes[p].getBuffer();
            for (int row = 0; row < rows; row++) {
                for (int column = 0; column < columns; column++) {
                    int at = row * planes[p].getRowStride() + column * planes[p].getPixelStride();
                    byte value = (byte) (p * 61 + row * 7 + column * 13);
                    if (!check) {
                        buffer.put(at, value);
                    } else if (buffer.get(at) != value) {
                        same = false;
                    }
                }
            }
        }
        return same;
    }

    private void yuv() throws Exception {
        ImageReader reader = reader(64, 48, ImageFormat.YUV_420_888, 2);
        ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 1);
        Log.i(TAG, "YUV writer: " + describe(writer));
        mFrames.drainPermits();
        try {
            Image image = take(writer);
            Log.i(TAG, "YUV dequeue: format " + image.getFormat() + ", " + image.getWidth() + " x "
                    + image.getHeight() + ", " + yuvPlanes(image));
            yuvPattern(image, false);
            image.setTimestamp(4242);
            writer.queueInputImage(image);
            Log.i(TAG, "YUV onImageAvailable: " + await(mFrames, 1));
            Image got = reader.acquireNextImage();
            if (got == null) {
                Log.i(TAG, "YUV acquire: null");
            } else {
                Log.i(TAG, "YUV acquire: format " + got.getFormat() + ", " + got.getWidth() + " x "
                        + got.getHeight() + ", timestamp " + got.getTimestamp() + ", " + yuvPlanes(got)
                        + ", the pattern back " + yuvPattern(got, true));
                got.close();
            }
        } catch (RuntimeException e) {
            Log.i(TAG, "YUV: " + e.getClass().getSimpleName() + " " + e.getMessage());
        }
        writer.close();
        reader.close();
        ImageReader canvasReader = reader(64, 48, ImageFormat.YUV_420_888, 2);
        Surface surface = canvasReader.getSurface();
        String locked;
        try {
            Canvas canvas = surface.lockCanvas(null);
            locked = "a canvas of " + canvas.getWidth() + " x " + canvas.getHeight();
            surface.unlockCanvasAndPost(canvas);
        } catch (RuntimeException e) {
            locked = e.getClass().getSimpleName() + " " + e.getMessage();
        }
        Log.i(TAG, "lockCanvas on a YUV reader's surface: " + locked);
        canvasReader.close();
    }
}
