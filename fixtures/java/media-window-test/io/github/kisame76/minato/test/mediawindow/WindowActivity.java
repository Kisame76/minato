package io.github.kisame76.minato.test.mediawindow;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;

/**
 * ADR-0060 e2c3's test (media-window-test): MediaCodec's H.264 decoder rendering into a window, a SurfaceView's
 * surface, on Minato's own baseline stream (tools/make-h264-sample.sh, 320 x 180, 30 frames). Android shows a
 * window's frames without a way back to the app's bytes, so the lines are the API's answers; graphics/host's test
 * checks the pixels. On a worker thread, once the SurfaceView's surface has its size:
 * - "window": every frame released with render, one every 34 ms, and OnFrameRenderedListener's callbacks on a
 *   HandlerThread: whether some came, each for a released frame's time, in order, after its release (how many to the
 *   device tag: Android may leave a frame out);
 * - "window at a time": five frames released with releaseOutputBuffer(index, time) 100 ms ahead, whether a callback
 *   came before its time (less a frame of 34 ms);
 * - "switch": setOutputSurface from the window to an ImageReader in YUV_420_888 after ten frames, each image's time
 *   and samples (CRC32 of Y, U and V inside the crop) logged, then back to the window after ten more;
 * - "producers": while the codec draws into the window, what the holder's lockCanvas, Surface.lockCanvas, EGL's
 *   window surface and a Vulkan swapchain on it (the JNI part) answer, and setVideoScalingMode with a mode it knows
 *   and one it does not;
 * - "gone": the SurfaceView made GONE while the codec renders, what three renders into the destroyed surface answer,
 *   then VISIBLE again and setOutputSurface to the new surface, and three renders after it;
 * - "ndk": the same window through AMediaCodec (the JNI part): AMediaCodec_setOnFrameRenderedCallback, a CPU lock and
 *   a second codec while it draws, setOutputSurface to an AImageReader's window and back;
 * - "after": the holder's lockCanvas once no codec draws into the window.
 */
public class WindowActivity extends Activity {
    private static final String TAG = "media-window-test";
    private static final String DEVICE_TAG = TAG + "-device";
    private static final String AVC = MediaFormat.MIMETYPE_VIDEO_AVC;
    private static final long FRAME_MS = 34;

    static {
        System.loadLibrary("media-window-test");
    }

    /** A Vulkan swapchain on the surface while a codec draws into it: the answers as a line. */
    private static native String swapchainNative(Surface surface);

    /** The window through AMediaCodec: its lines, those that start with "device " for the device tag. */
    private static native String ndkNative(String decoder, byte[] stream, long[] index, Surface surface);

    private SurfaceView mView;
    private volatile CountDownLatch mCreated = new CountDownLatch(1);
    private volatile CountDownLatch mDestroyed = new CountDownLatch(1);
    private boolean mStarted;
    private String mDecoder;
    private byte[] mBytes;
    private final List<long[]> mUnits = new ArrayList<>();
    private HandlerThread mCallbacks;
    private Handler mHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new SurfaceView(this);
        mView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Log.i(TAG, "surface created");
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.i(TAG + "-device", "surface changed: format " + format + ", " + width + " x " + height);
                mCreated.countDown();
                if (!mStarted) {
                    mStarted = true;
                    new Thread(WindowActivity.this::run, "tests").start();
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "surface destroyed");
                mDestroyed.countDown();
            }
        });
        setContentView(mView);
    }

    private void run() {
        try {
            mCallbacks = new HandlerThread("rendered");
            mCallbacks.start();
            mHandler = new Handler(mCallbacks.getLooper());
            load();
            pick();
            Surface surface = mView.getHolder().getSurface();
            part("window", () -> window("window", surface));
            part("window at a time", () -> atTime("window at a time", surface));
            part("switch", () -> switched("switch", surface));
            part("producers", () -> producers("producers", surface));
            part("gone", () -> gone("gone"));
            part("ndk", () -> logNative(ndkNative(mDecoder, mBytes, flatten(), mView.getHolder().getSurface())));
            part("after", () -> after("after"));
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(DEVICE_TAG, Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private interface Part {
        void run() throws Exception;
    }

    /** A part of the test; one that fails is logged by its exception, and the next one runs. */
    private void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t.getClass().getName());
            Log.i(DEVICE_TAG, name + ": " + Log.getStackTraceString(t));
        }
    }

    private void logNative(String lines) {
        for (String line : lines.split("\n")) {
            if (line.isEmpty()) continue;
            if (line.startsWith("device ")) {
                Log.i(DEVICE_TAG, line.substring(7));
            } else {
                Log.i(TAG, line);
            }
        }
    }

    private void load() throws IOException {
        mBytes = read(getAssets().open("baseline.h264"));
        for (String line : new String(read(getAssets().open("baseline.idx")), "UTF-8").split("\n")) {
            if (line.isEmpty()) continue;
            String[] parts = line.split(" ");
            mUnits.add(new long[] {Long.parseLong(parts[0]), Long.parseLong(parts[1]), Long.parseLong(parts[2]),
                    Long.parseLong(parts[3])});
        }
    }

    private static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] chunk = new byte[8192];
        for (int n; (n = in.read(chunk)) > 0; ) {
            out.write(chunk, 0, n);
        }
        in.close();
        return out.toByteArray();
    }

    private long[] flatten() {
        long[] all = new long[mUnits.size() * 4];
        for (int i = 0; i < mUnits.size(); i++) {
            System.arraycopy(mUnits.get(i), 0, all, i * 4, 4);
        }
        return all;
    }

    /** The first decoder of video/avc that is not the emulator's goldfish one, as media-codec-test picks it. */
    private void pick() {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder()) continue;
            for (String type : info.getSupportedTypes()) {
                if (type.equalsIgnoreCase(AVC) && mDecoder == null && !info.getName().contains(".goldfish.")) {
                    mDecoder = info.getName();
                }
            }
        }
        Log.i(TAG, "a decoder: " + (mDecoder != null));
        Log.i(DEVICE_TAG, "the test's decoder: " + mDecoder);
    }

    /** The frames' render callbacks of one codec: each frame's time and the time it was rendered. */
    private static final class Rendered implements MediaCodec.OnFrameRenderedListener {
        final List<long[]> frames = new ArrayList<>();

        @Override
        public synchronized void onFrameRendered(MediaCodec codec, long presentationTimeUs, long nanoTime) {
            frames.add(new long[] {presentationTimeUs, nanoTime});
        }

        synchronized List<long[]> copy() {
            return new ArrayList<>(frames);
        }
    }

    private MediaCodec codec(Surface surface, Rendered rendered) throws IOException {
        MediaCodec codec = MediaCodec.createByCodecName(mDecoder);
        if (rendered != null) codec.setOnFrameRenderedListener(rendered, mHandler);
        codec.configure(MediaFormat.createVideoFormat(AVC, 320, 180), surface, null, 0);
        codec.start();
        return codec;
    }

    /** A decoding codec: units in as it takes them, the next frame out within two seconds. */
    private final class Feeder {
        final MediaCodec codec;
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int next;
        boolean sentEnd;

        Feeder(MediaCodec codec) {
            this.codec = codec;
        }

        /** The next output buffer's index with `info` set, -1 at the end of the stream or after two seconds. */
        int frame() {
            long deadline = System.currentTimeMillis() + 2000;
            while (System.currentTimeMillis() < deadline) {
                if (!sentEnd) {
                    int in = codec.dequeueInputBuffer(10000);
                    if (in >= 0) {
                        if (next < mUnits.size()) {
                            long[] u = mUnits.get(next++);
                            ByteBuffer input = codec.getInputBuffer(in);
                            input.clear();
                            input.put(mBytes, (int) u[0], (int) u[1]);
                            codec.queueInputBuffer(in, 0, (int) u[1], u[2], 0);
                        } else {
                            codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            sentEnd = true;
                        }
                    }
                }
                int out = codec.dequeueOutputBuffer(info, 10000);
                if (out >= 0) {
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0 && info.size == 0) {
                        codec.releaseOutputBuffer(out, false);
                        return -1;
                    }
                    return out;
                }
            }
            return -1;
        }
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * The callbacks against the frames released: whether some came, each for a released time, in increasing order,
     * and whether each one's render time is its frame's time in nanoseconds, the buffer's timestamp, as Android's
     * are; how many, and the render times against the release, to the device tag.
     */
    private void logRendered(String name, Rendered rendered, List<long[]> released) {
        pause(500);
        List<long[]> frames = rendered.copy();
        boolean known = true, ordered = true, stamped = true;
        long last = -1;
        for (long[] frame : frames) {
            boolean found = false;
            for (long[] r : released) {
                found |= r[0] == frame[0];
            }
            known &= found;
            ordered &= frame[0] > last;
            stamped &= frame[1] == frame[0] * 1000;
            last = frame[0];
        }
        Log.i(TAG, name + ": " + released.size() + " frames released, callbacks: some " + !frames.isEmpty()
                + ", each a released frame's " + known + ", in order " + ordered + ", its time the frame's "
                + stamped);
        StringBuilder device = new StringBuilder(name + ": " + frames.size() + " callbacks, rendered after the release,"
                + " in ms:");
        for (long[] frame : frames) {
            for (long[] r : released) {
                if (r[0] == frame[0]) device.append(' ').append((frame[1] - r[1]) / 1_000_000L);
            }
        }
        Log.i(DEVICE_TAG, device.toString());
    }

    private void window(String name, Surface surface) throws IOException {
        Rendered rendered = new Rendered();
        MediaCodec codec = codec(surface, rendered);
        Feeder feeder = new Feeder(codec);
        List<long[]> released = new ArrayList<>();
        for (int out; (out = feeder.frame()) >= 0; ) {
            released.add(new long[] {feeder.info.presentationTimeUs, System.nanoTime()});
            codec.releaseOutputBuffer(out, true);
            pause(FRAME_MS);
        }
        MediaFormat format = codec.getOutputFormat();
        Log.i(TAG, name + ": output format " + format.getInteger(MediaFormat.KEY_WIDTH) + " x "
                + format.getInteger(MediaFormat.KEY_HEIGHT) + ", color format "
                + Integer.toHexString(format.getInteger(MediaFormat.KEY_COLOR_FORMAT)));
        logRendered(name, rendered, released);
        codec.stop();
        codec.release();
    }

    private void atTime(String name, Surface surface) throws IOException {
        Rendered rendered = new Rendered();
        MediaCodec codec = codec(surface, rendered);
        Feeder feeder = new Feeder(codec);
        List<long[]> asked = new ArrayList<>();
        for (int out, n = 0; n < 5 && (out = feeder.frame()) >= 0; n++) {
            long at = System.nanoTime() + 100_000_000L;
            asked.add(new long[] {feeder.info.presentationTimeUs, at});
            codec.releaseOutputBuffer(out, at);
            pause(FRAME_MS);
        }
        pause(500);
        boolean askedTime = true;
        StringBuilder device = new StringBuilder(name + ": rendered after the time asked, in ms:");
        for (long[] frame : rendered.copy()) {
            for (long[] a : asked) {
                if (a[0] != frame[0]) continue;
                askedTime &= frame[1] == a[1];
                device.append(' ').append((frame[1] - a[1]) / 1_000_000L);
            }
        }
        Log.i(TAG, name + ": callbacks: some " + !rendered.copy().isEmpty() + ", each one's time the time asked "
                + askedTime);
        Log.i(DEVICE_TAG, device.toString());
        codec.stop();
        codec.release();
    }

    /** The image's time in microseconds and the CRC32 of each plane's samples inside the crop; closes it. */
    private static String samples(Image image) {
        Rect crop = image.getCropRect();
        StringBuilder line = new StringBuilder("pts " + image.getTimestamp() / 1000 + ", " + crop.width() + " x "
                + crop.height());
        Image.Plane[] planes = image.getPlanes();
        for (int p = 0; p < planes.length; p++) {
            int shift = p == 0 ? 0 : 1;
            CRC32 crc = new CRC32();
            ByteBuffer buffer = planes[p].getBuffer();
            for (int y = crop.top >> shift; y < (crop.bottom + shift) >> shift; y++) {
                for (int x = crop.left >> shift; x < (crop.right + shift) >> shift; x++) {
                    crc.update(buffer.get(y * planes[p].getRowStride() + x * planes[p].getPixelStride()));
                }
            }
            line.append(p == 0 ? ", Y " : p == 1 ? " U " : " V ").append(Long.toHexString(crc.getValue()));
        }
        image.close();
        return line.toString();
    }

    private void switched(String name, Surface surface) throws Exception {
        ImageReader reader = ImageReader.newInstance(320, 180, ImageFormat.YUV_420_888, 3);
        Rendered rendered = new Rendered();
        MediaCodec codec = codec(surface, rendered);
        Feeder feeder = new Feeder(codec);
        List<long[]> released = new ArrayList<>();
        // The frame right after the switch may come out of the window's last buffer and miss the reader (Android's
        // does now and then), so the reader's images are counted on the device tag.
        List<Long> toReaderTimes = new ArrayList<>(), imageTimes = new ArrayList<>();
        int frames = 0, images = 0;
        for (int out; (out = feeder.frame()) >= 0; frames++) {
            if (frames == 10) {
                codec.setOutputSurface(reader.getSurface());
                Log.i(TAG, name + ": to the reader");
            } else if (frames == 20) {
                codec.setOutputSurface(surface);
                Log.i(TAG, name + ": back to the window");
            }
            boolean toReader = frames >= 10 && frames < 20;
            released.add(new long[] {feeder.info.presentationTimeUs, System.nanoTime()});
            if (toReader) toReaderTimes.add(feeder.info.presentationTimeUs);
            codec.releaseOutputBuffer(out, true);
            if (toReader) {
                Image image = null;
                for (long wait = System.currentTimeMillis() + 500; image == null
                        && System.currentTimeMillis() < wait; ) {
                    image = reader.acquireNextImage();
                    if (image == null) Thread.sleep(2);
                }
                if (image != null) {
                    imageTimes.add(image.getTimestamp() / 1000);
                    Log.i(DEVICE_TAG, name + ": " + samples(image));
                    images++;
                }
            }
            pause(FRAME_MS);
        }
        boolean known = !imageTimes.isEmpty(), ordered = true;
        long last = -1;
        for (long time : imageTimes) {
            known &= toReaderTimes.contains(time);
            ordered &= time > last;
            last = time;
        }
        Log.i(TAG, name + ": " + frames + " frames, the reader's images: some " + (images > 0)
                + ", each a frame rendered into it " + known + ", in order " + ordered);
        Log.i(DEVICE_TAG, name + ": " + images + " images of 10");
        logRendered(name, rendered, released);
        codec.stop();
        codec.release();
        reader.close();
    }

    /** What lockCanvas, an EGL window surface and a Vulkan swapchain answer on a codec's window. */
    private void producers(String name, Surface surface) throws IOException {
        MediaCodec codec = codec(surface, null);
        Feeder feeder = new Feeder(codec);
        int out = feeder.frame();
        if (out >= 0) codec.releaseOutputBuffer(out, true);
        Canvas canvas = mView.getHolder().lockCanvas();
        Log.i(TAG, name + ": the holder's lockCanvas: " + (canvas != null ? "a canvas" : "null"));
        if (canvas != null) mView.getHolder().unlockCanvasAndPost(canvas);
        try {
            Canvas locked = surface.lockCanvas(null);
            Log.i(TAG, name + ": Surface.lockCanvas: a canvas");
            surface.unlockCanvasAndPost(locked);
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": Surface.lockCanvas: " + e.getClass().getName());
        }
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                EGL14.EGL_WINDOW_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        EGLSurface window = count[0] > 0
                ? EGL14.eglCreateWindowSurface(display, configs[0], surface, new int[] {EGL14.EGL_NONE}, 0)
                : EGL14.EGL_NO_SURFACE;
        int error = EGL14.eglGetError();
        Log.i(TAG, name + ": an EGL window surface: " + (window != EGL14.EGL_NO_SURFACE ? "made" : "none"));
        Log.i(DEVICE_TAG, name + ": the EGL window surface's error 0x" + Integer.toHexString(error));
        if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window);
        Log.i(TAG, name + ": " + swapchainNative(surface));
        try {
            codec.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING);
            Log.i(TAG, name + ": setVideoScalingMode: ok");
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": setVideoScalingMode: " + e.getClass().getName());
        }
        try {
            codec.setVideoScalingMode(3);
            Log.i(TAG, name + ": setVideoScalingMode(3): ok");
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": setVideoScalingMode(3): " + e.getClass().getName() + ": " + e.getMessage());
        }
        codec.stop();
        codec.release();
    }

    /** What renders into a destroyed surface answer, and setOutputSurface to the new one. */
    private void gone(String name) throws Exception {
        Surface surface = mView.getHolder().getSurface();
        Rendered rendered = new Rendered();
        MediaCodec codec = codec(surface, rendered);
        Feeder feeder = new Feeder(codec);
        for (int n = 0; n < 3; n++) {
            int out = feeder.frame();
            if (out >= 0) codec.releaseOutputBuffer(out, true);
            pause(FRAME_MS);
        }
        mDestroyed = new CountDownLatch(1);
        runOnUiThread(() -> mView.setVisibility(View.GONE));
        Log.i(TAG, name + ": surface destroyed in time " + mDestroyed.await(5, TimeUnit.SECONDS));
        // Android's codec fails a render into it (CodecException) and is released soon after, by itself, so that
        // a call that comes later answers IllegalStateException: which one comes first is timing.
        boolean failed = false, released = false;
        for (int n = 0; n < 3; n++) {
            try {
                int out = feeder.frame();
                if (out >= 0) codec.releaseOutputBuffer(out, true);
            } catch (RuntimeException e) {
                failed = true;
                released |= e instanceof IllegalStateException && String.valueOf(e.getMessage()).contains("Released");
                Log.i(DEVICE_TAG, name + ": " + e);
            }
            pause(FRAME_MS);
        }
        Log.i(TAG, name + ": renders into the destroyed surface fail " + failed + ", the codec released by itself "
                + released);
        int before = rendered.copy().size();
        mCreated = new CountDownLatch(1);
        runOnUiThread(() -> mView.setVisibility(View.VISIBLE));
        Log.i(TAG, name + ": surface again in time " + mCreated.await(5, TimeUnit.SECONDS));
        try {
            codec.setOutputSurface(mView.getHolder().getSurface());
            Log.i(TAG, name + ": setOutputSurface to the new surface: ok");
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": setOutputSurface to the new surface: " + e.getClass().getName());
        }
        StringBuilder after = new StringBuilder(name + ": renders after it:");
        for (int n = 0; n < 3; n++) {
            try {
                int out = feeder.frame();
                if (out < 0) {
                    after.append(" no frame");
                    continue;
                }
                codec.releaseOutputBuffer(out, true);
                after.append(" ok");
            } catch (RuntimeException e) {
                after.append(' ').append(e.getClass().getSimpleName());
            }
            pause(FRAME_MS);
        }
        pause(500);
        Log.i(TAG, after.toString());
        Log.i(TAG, name + ": callbacks after the new surface " + (rendered.copy().size() > before));
        try {
            codec.stop();
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": stop: " + e.getClass().getSimpleName());
        }
        codec.release();
    }

    /** The holder's lockCanvas once no codec draws into the window. */
    private void after(String name) {
        Canvas canvas = mView.getHolder().lockCanvas();
        Log.i(TAG, name + ": the holder's lockCanvas: " + (canvas != null ? "a canvas" : "null"));
        if (canvas != null) mView.getHolder().unlockCanvasAndPost(canvas);
    }
}
