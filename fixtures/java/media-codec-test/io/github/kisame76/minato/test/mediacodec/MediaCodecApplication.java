package io.github.kisame76.minato.test.mediacodec;

import android.app.Application;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;

/**
 * ADR-0060 e2b's test of MediaCodec's H.264 decoder in ByteBuffer mode, on Minato's own streams
 * (tools/make-h264-sample.sh: baseline.h264 and main.h264 in its assets, with their indexes). H.264's decoding is
 * exact by the standard, so every decoder gives the same samples: each frame's time, flags and a CRC32 of its Y, U and
 * V samples inside the crop, walked by the image's row and pixel strides, are logged, so that a planar and a
 * semi-planar decoder compare. From Application.attachBaseContext:
 * - MediaCodecList: whether it has a decoder of video/avc (its name, color formats and strides to the device tag).
 *   The test decodes with the first one that is not the emulator's goldfish decoder, which hands frames out in decode
 *   order, as no device's decoder does (AOSP's c2.android.avc.decoder hands them out in presentation order, with the
 *   same samples);
 * - baseline and main decoded synchronously, main with B-frames, whose frames come out in presentation order; main
 *   again with as many units queued as the codec takes before any frame is taken (how many is the codec's, not
 *   logged), so that a codec whose decoder is behind keeps every unit;
 * - baseline decoded asynchronously (setCallback on a HandlerThread), as ExoPlayer decodes on API 31 and later;
 * - a flush after 5 of the first 15 units' frames (AOSP's decoder holds the others until more input comes), then
 *   the stream again from its second key frame;
 * - the refusals: an unknown type, and dequeueOutputBuffer before start, each by its exception's class;
 * - main through the NDK's AMediaCodec in the app's JNI library, its output buffers walked by the output format;
 * - ADR-0060 e2c: baseline rendered into an ImageReader's Surface in YUV_420_888, and main into an AImageReader's
 *   window through AMediaCodec, each image's time and samples logged as the buffers' are;
 * - ADR-0060 e2c2: baseline rendered into a SurfaceTexture's Surface, each frame drawn through samplerExternalOES
 *   with the texture's matrix into a pbuffer of its size and read back: its time is logged, and, as its colours are
 *   the device's conversion, only which of four matrices (BT.601 and BT.709, video and full range) of the
 *   frame's own samples (ByteBuffer mode) comes closest and whether its mean difference stays within 3; the
 *   differences and the matrix, which has the buffer's padding (the emulator's are 384 wide), go to the device tag. (An RGBA ImageReader is no reference: Android's codec queues YV12 into it, and
 *   acquireNextImage throws.)
 * Each wait is bounded; it logs with the tag media-codec-test and ends with "done".
 */
public class MediaCodecApplication extends Application {
    private static final String TAG = "media-codec-test";
    private static final String AVC = MediaFormat.MIMETYPE_VIDEO_AVC;

    static {
        System.loadLibrary("media-codec-test");
    }

    /** Decodes the stream through AMediaCodec with the named decoder; its log lines, joined by newlines. */
    private static native String decodeNative(String decoder, byte[] stream, long[] index);

    /** The same through an AImageReader's window in YUV_420_888, each frame rendered and its image logged. */
    private static native String decodeToReaderNative(String decoder, byte[] stream, long[] index);

    /** A reader of one image whose first the test holds while the codec renders the rest. */
    private static native String holdNative(String decoder, byte[] stream, long[] index);

    /** A reader in RGB_565, a format no decoder draws. */
    private static native String rgb565Native(String decoder, byte[] stream, long[] index);

    /** The decoder of video/avc the test uses (list()'s). */
    private static String sDecoder;

    /** The last output format logged, by its name, size and crop. */
    private static String sLastFormat;

    /** A stream and its access units: offsets, sizes, times and key flags. */
    private static final class Stream {
        byte[] bytes;
        final List<long[]> units = new ArrayList<>();
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            flags();
            list();
            Stream baseline = load(base, "baseline");
            Stream main = load(base, "main");
            decode("baseline", baseline);
            decode("main", main);
            decodeAhead("main ahead", main);
            decodeAsync("baseline async", baseline);
            flushed("baseline flushed", baseline);
            refusals();
            logNative(decodeNative(sDecoder, main.bytes, flatten(main)));
            decodeToReader("baseline reader", baseline);
            decodeToTexture("baseline texture", baseline);
            logNative(decodeToReaderNative(sDecoder, main.bytes, flatten(main)));
            logNative(holdNative(sDecoder, baseline.bytes, flatten(baseline)));
            logNative(rgb565Native(sDecoder, baseline.bytes, flatten(baseline)));
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    /** The JNI part's lines, those that start with "device " to the device tag. */
    private static void logNative(String lines) {
        for (String line : lines.split("\n")) {
            if (line.startsWith("device ")) {
                Log.i(TAG + "-device", line.substring(7));
            } else {
                Log.i(TAG, line);
            }
        }
    }

    private static Stream load(Context context, String name) throws IOException {
        Stream stream = new Stream();
        stream.bytes = read(context.getAssets().open(name + ".h264"));
        for (String line : new String(read(context.getAssets().open(name + ".idx")), "UTF-8").split("\n")) {
            if (line.isEmpty()) continue;
            String[] parts = line.split(" ");
            stream.units.add(new long[] {Long.parseLong(parts[0]), Long.parseLong(parts[1]), Long.parseLong(parts[2]),
                    Long.parseLong(parts[3])});
        }
        return stream;
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

    private static long[] flatten(Stream stream) {
        long[] all = new long[stream.units.size() * 4];
        for (int i = 0; i < stream.units.size(); i++) {
            System.arraycopy(stream.units.get(i), 0, all, i * 4, 4);
        }
        return all;
    }

    /** The media codec feature flags MediaCodec asks at run time, by reflection, to the device tag. */
    private static void flags() {
        StringBuilder line = new StringBuilder("flags:");
        for (String name : new String[] {"dynamicColorAspects", "hlgEditing", "inProcessSwAudioCodec",
                "nullOutputSurface", "nullOutputSurfaceSupport", "regionOfInterest", "setCallbackStall"}) {
            try {
                Object value = Class.forName("android.media.codec.Flags").getMethod(name).invoke(null);
                line.append(' ').append(name).append('=').append(value);
            } catch (ReflectiveOperationException | RuntimeException e) {
                line.append(' ').append(name).append(": ").append(e.getClass().getSimpleName());
            }
        }
        Log.i(TAG + "-device", line.toString());
    }

    private static void list() {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        int decoders = 0;
        for (MediaCodecInfo info : list.getCodecInfos()) {
            if (info.isEncoder()) continue;
            for (String type : info.getSupportedTypes()) {
                if (!type.equalsIgnoreCase(AVC)) continue;
                decoders++;
                if (sDecoder == null && !info.getName().contains(".goldfish.")) sDecoder = info.getName();
                MediaCodecInfo.CodecCapabilities capabilities = info.getCapabilitiesForType(type);
                StringBuilder formats = new StringBuilder();
                for (int format : capabilities.colorFormats) {
                    formats.append(' ').append(Integer.toHexString(format));
                }
                Log.i(TAG + "-device", "decoder " + info.getName() + ", color formats" + formats);
            }
        }
        MediaFormat format = MediaFormat.createVideoFormat(AVC, 320, 180);
        Log.i(TAG, "a decoder of video/avc: " + (decoders > 0) + ", findDecoderForFormat: "
                + (list.findDecoderForFormat(format) != null) + ", one that is not goldfish: " + (sDecoder != null));
        Log.i(TAG + "-device", "the test's decoder: " + sDecoder);
    }

    private static MediaFormat format() {
        return MediaFormat.createVideoFormat(AVC, 320, 180);
    }

    /**
     * The output format's crop and size, as every decoder gives them, when they differ from the last one's (a decoder
     * may announce the same format again); the rest to the device tag.
     */
    private static void logFormat(String name, MediaFormat format) {
        String key = name + ": " + format.getInteger(MediaFormat.KEY_WIDTH) + " x "
                + format.getInteger(MediaFormat.KEY_HEIGHT) + crop(format);
        if (key.equals(sLastFormat)) return;
        sLastFormat = key;
        Log.i(TAG, name + ": output format " + format.getString(MediaFormat.KEY_MIME) + ", "
                + format.getInteger(MediaFormat.KEY_WIDTH) + " x " + format.getInteger(MediaFormat.KEY_HEIGHT)
                + crop(format));
        Log.i(TAG + "-device", name + ": " + format);
    }

    private static String crop(MediaFormat format) {
        if (!format.containsKey("crop-left")) return ", no crop";
        return ", crop " + format.getInteger("crop-left") + " " + format.getInteger("crop-top") + " "
                + format.getInteger("crop-right") + " " + format.getInteger("crop-bottom");
    }

    /** One frame's time, flags and the CRC32 of each plane's samples inside the crop. */
    private static String frame(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
        Image image = codec.getOutputImage(index);
        if (image == null) return "pts " + info.presentationTimeUs + ", no image";
        return samples(new StringBuilder("pts " + info.presentationTimeUs + ", flags " + info.flags + ", "), image);
    }

    /** The image's crop, format and the CRC32 of each plane's samples inside the crop, after `line`; closes it. */
    private static String samples(StringBuilder line, Image image) {
        Rect crop = image.getCropRect();
        line.append(crop.width() + " x " + crop.height() + ", format " + image.getFormat());
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

    private static void queue(MediaCodec codec, int index, Stream stream, int unit) {
        ByteBuffer input = codec.getInputBuffer(index);
        long[] u = stream.units.get(unit);
        input.clear();
        input.put(stream.bytes, (int) u[0], (int) u[1]);
        codec.queueInputBuffer(index, 0, (int) u[1], u[2], 0);
    }

    /**
     * Decodes units [first, last) synchronously, with the end of the stream after them when `end`, until the end comes
     * or, without it, until `wanted` frames came, so that a decoder's latency does not change what is logged; whether
     * the end came.
     */
    private static boolean run(String name, MediaCodec codec, Stream stream, int first, int last, boolean end,
            int wanted) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int next = first, frames = 0;
        boolean ended = false, sentEnd = false;
        long deadline = System.currentTimeMillis() + 10000;
        while (System.currentTimeMillis() < deadline) {
            if (next < last || (end && !sentEnd)) {
                int in = codec.dequeueInputBuffer(10000);
                if (in >= 0) {
                    if (next < last) {
                        queue(codec, in, stream, next++);
                    } else {
                        codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        sentEnd = true;
                    }
                }
            }
            int out = codec.dequeueOutputBuffer(info, 10000);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                logFormat(name, codec.getOutputFormat());
            } else if (out >= 0) {
                if (info.size > 0 || (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) {
                    Log.i(TAG, name + ": " + frame(codec, out, info));
                    frames++;
                }
                codec.releaseOutputBuffer(out, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    ended = true;
                    break;
                }
            }
            if (!end && frames >= wanted) break;
        }
        Log.i(TAG, name + ": " + frames + " frames" + (ended ? ", end of stream" : ""));
        return ended;
    }

    /**
     * Decodes the stream into an ImageReader's Surface of its size in YUV_420_888: each frame rendered at its time, and
     * the reader's next image logged with its time (in microseconds) and samples, as a frame of ByteBuffer mode is.
     */
    private static void decodeToReader(String name, Stream stream) throws Exception {
        ImageReader reader = ImageReader.newInstance(320, 180, ImageFormat.YUV_420_888, 3);
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), reader.getSurface(), null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int next = 0, frames = 0;
        boolean ended = false, sentEnd = false;
        long deadline = System.currentTimeMillis() + 10000;
        while (!ended && System.currentTimeMillis() < deadline) {
            if (!sentEnd) {
                int in = codec.dequeueInputBuffer(10000);
                if (in >= 0) {
                    if (next < stream.units.size()) {
                        queue(codec, in, stream, next++);
                    } else {
                        codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        sentEnd = true;
                    }
                }
            }
            int out = codec.dequeueOutputBuffer(info, 10000);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                logFormat(name, codec.getOutputFormat());
            } else if (out >= 0) {
                ended = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                boolean render = !ended || info.size > 0;  // a frame, whatever size a surface's buffer has
                if (frames == 0) Log.i(TAG + "-device", name + ": a rendered buffer's size " + info.size);
                codec.releaseOutputBuffer(out, render);
                if (!render) continue;
                Image image = null;
                for (long wait = System.currentTimeMillis() + 2000; image == null
                        && System.currentTimeMillis() < wait; ) {
                    image = reader.acquireNextImage();
                    if (image == null) Thread.sleep(2);
                }
                if (image == null) {
                    Log.i(TAG, name + ": pts " + info.presentationTimeUs + ", no image");
                } else {
                    long time = image.getTimestamp() / 1000;
                    Log.i(TAG, name + ": " + samples(new StringBuilder("pts " + time + ", "), image));
                    frames++;
                }
            }
        }
        Log.i(TAG, name + ": " + frames + " frames" + (ended ? ", end of stream" : ""));
        codec.stop();
        codec.release();
        reader.close();
    }

    /** The stream's frames in ByteBuffer mode by their time: Y (320 x 180), then U and V (160 x 90 each). */
    private static Map<Long, byte[]> samplesByTime(Stream stream) throws IOException {
        Map<Long, byte[]> frames = new HashMap<>();
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), null, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int next = 0;
        boolean sentEnd = false;
        long deadline = System.currentTimeMillis() + 10000;
        while (System.currentTimeMillis() < deadline) {
            if (!sentEnd) {
                int in = codec.dequeueInputBuffer(10000);
                if (in >= 0) {
                    if (next < stream.units.size()) {
                        queue(codec, in, stream, next++);
                    } else {
                        codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        sentEnd = true;
                    }
                }
            }
            int out = codec.dequeueOutputBuffer(info, 10000);
            if (out < 0) continue;
            Image image = info.size > 0 ? codec.getOutputImage(out) : null;
            if (image != null) {
                byte[] samples = new byte[320 * 180 + 2 * 160 * 90];
                Image.Plane[] planes = image.getPlanes();
                for (int p = 0, at = 0; p < 3; p++) {
                    int w = p == 0 ? 320 : 160, h = p == 0 ? 180 : 90;
                    ByteBuffer buffer = planes[p].getBuffer();
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            samples[at++] = buffer.get(y * planes[p].getRowStride() + x * planes[p].getPixelStride());
                        }
                    }
                }
                image.close();
                frames.put(info.presentationTimeUs, samples);
            }
            codec.releaseOutputBuffer(out, false);
            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
        }
        codec.stop();
        codec.release();
        return frames;
    }

    private static final String[] MATRICES = {"BT.601 video", "BT.709 video", "BT.601 full", "BT.709 full"};

    /**
     * The mean absolute difference of a read-back frame (rows bottom up, as glReadPixels gives them, of an upright
     * image) from the samples converted by each of MATRICES.
     */
    private static double[] differences(ByteBuffer rgba, byte[] samples) {
        double[] sums = new double[MATRICES.length];
        long count = 0;
        for (int y = 0; y < 180; y++) {
            for (int x = 0; x < 320; x++) {
                int luma = samples[y * 320 + x] & 0xff;
                int cb = (samples[320 * 180 + (y / 2) * 160 + x / 2] & 0xff) - 128;
                int cr = (samples[320 * 180 + 160 * 90 + (y / 2) * 160 + x / 2] & 0xff) - 128;
                int at = ((179 - y) * 320 + x) * 4;
                int[] got = {rgba.get(at) & 0xff, rgba.get(at + 1) & 0xff, rgba.get(at + 2) & 0xff};
                for (int m = 0; m < MATRICES.length; m++) {
                    boolean video = m < 2, bt709 = m % 2 == 1;
                    double yy = video ? (luma - 16) * 255.0 / 219 : luma;
                    double scale = video ? 255.0 / 224 : 1.0;
                    double u = cb * scale, v = cr * scale;
                    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
                    double r = yy + 2 * (1 - kr) * v;
                    double b = yy + 2 * (1 - kb) * u;
                    double g = (yy - kr * r - kb * b) / kg;
                    double[] want = {r, g, b};
                    for (int c = 0; c < 3; c++) {
                        sums[m] += Math.abs(Math.max(0, Math.min(255, want[c])) - got[c]);
                    }
                }
                count += 3;
            }
        }
        for (int m = 0; m < sums.length; m++) sums[m] /= count;
        return sums;
    }

    private static final String TEXTURE_VERTEX = "attribute vec2 position;\n"
            + "uniform mat4 transform;\n"
            + "varying vec2 coordinate;\n"
            + "void main() {\n"
            + "  coordinate = (transform * vec4(position * 0.5 + 0.5, 0.0, 1.0)).xy;\n"
            + "  gl_Position = vec4(position, 0.0, 1.0);\n"
            + "}\n";
    private static final String TEXTURE_FRAGMENT = "#extension GL_OES_EGL_image_external : require\n"
            + "precision mediump float;\n"
            + "uniform samplerExternalOES image;\n"
            + "varying vec2 coordinate;\n"
            + "void main() { gl_FragColor = texture2D(image, coordinate); }\n";

    private static int shader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        return shader;
    }

    /**
     * Decodes the stream into a SurfaceTexture's Surface (its default buffer size the stream's): each frame rendered,
     * then on the texture's GL thread taken by updateTexImage, drawn with its matrix into a pbuffer of its size and
     * read back, and logged with its time, its matrix, the closest of MATRICES and whether that one's mean difference
     * stays within 3 (the conversions round, and the device's chroma filter is its own).
     */
    private static void decodeToTexture(String name, Stream stream) throws Exception {
        Map<Long, byte[]> samples = samplesByTime(stream);
        HandlerThread thread = new HandlerThread("media-codec-test gl");
        thread.start();
        Handler gl = new Handler(thread.getLooper());
        Object[] made = new Object[4];  // display, pbuffer, context, texture
        int[] program = new int[1];
        float[] matrix = new float[16];
        long[] last = {-1};
        CountDownLatch setUp = new CountDownLatch(1);
        java.util.concurrent.Semaphore frames = new java.util.concurrent.Semaphore(0);
        gl.post(() -> {
            EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] version = new int[2];
            EGL14.eglInitialize(display, version, 0, version, 1);
            int[] spec = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                    EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, new int[1], 0);
            EGLSurface pbuffer = EGL14.eglCreatePbufferSurface(display, configs[0],
                    new int[] {EGL14.EGL_WIDTH, 320, EGL14.EGL_HEIGHT, 180, EGL14.EGL_NONE}, 0);
            EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            boolean current = EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context);
            Log.i(TAG + "-device", name + ": pbuffer " + (pbuffer != EGL14.EGL_NO_SURFACE) + ", context "
                    + (context != EGL14.EGL_NO_CONTEXT) + ", current " + current + ", error 0x"
                    + Integer.toHexString(EGL14.eglGetError()));
            program[0] = GLES20.glCreateProgram();
            GLES20.glAttachShader(program[0], shader(GLES20.GL_VERTEX_SHADER, TEXTURE_VERTEX));
            GLES20.glAttachShader(program[0], shader(GLES20.GL_FRAGMENT_SHADER, TEXTURE_FRAGMENT));
            GLES20.glBindAttribLocation(program[0], 0, "position");
            GLES20.glLinkProgram(program[0]);
            int[] names = new int[1];
            GLES20.glGenTextures(1, names, 0);
            SurfaceTexture texture = new SurfaceTexture(names[0]);
            texture.setDefaultBufferSize(320, 180);
            texture.setOnFrameAvailableListener(available -> {
                available.updateTexImage();
                long time = available.getTimestamp() / 1000;
                if (time == last[0]) return;  // a call without a new frame, which Android makes at times
                last[0] = time;
                available.getTransformMatrix(matrix);
                FloatBuffer quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
                quad.put(new float[] {-1, -1, 1, -1, -1, 1, 1, 1}).position(0);
                GLES20.glViewport(0, 0, 320, 180);
                GLES20.glClearColor(0, 0, 0, 0);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glUseProgram(program[0]);
                GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program[0], "transform"), 1, false, matrix, 0);
                GLES20.glUniform1i(GLES20.glGetUniformLocation(program[0], "image"), 0);
                GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, quad);
                GLES20.glEnableVertexAttribArray(0);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                ByteBuffer rgba = ByteBuffer.allocateDirect(320 * 180 * 4);
                GLES20.glReadPixels(0, 0, 320, 180, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
                byte[] frame = samples.get(time);
                if (frame == null) {
                    Log.i(TAG, name + ": pts " + time + ", no such frame");
                } else {
                    double[] mean = differences(rgba, frame);
                    int best = 0;
                    for (int m = 1; m < mean.length; m++) if (mean[m] < mean[best]) best = m;
                    Log.i(TAG, name + ": pts " + time + ", closest " + MATRICES[best] + ", within 3 "
                            + (mean[best] <= 3));
                    Log.i(TAG + "-device", name + ": pts " + time + ", matrix " + Arrays.toString(matrix)
                            + ", mean differences "
                            + String.format("%.2f %.2f %.2f %.2f", mean[0], mean[1], mean[2], mean[3])
                            + ", error 0x" + Integer.toHexString(GLES20.glGetError()));
                }
                frames.release();
            }, gl);
            made[0] = display;
            made[1] = pbuffer;
            made[2] = context;
            made[3] = texture;
            setUp.countDown();
        });
        setUp.await(10, TimeUnit.SECONDS);
        SurfaceTexture texture = (SurfaceTexture) made[3];
        Surface surface = new Surface(texture);
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), surface, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int next = 0, rendered = 0;
        boolean ended = false, sentEnd = false;
        long deadline = System.currentTimeMillis() + 20000;
        while (!ended && System.currentTimeMillis() < deadline) {
            if (!sentEnd) {
                int in = codec.dequeueInputBuffer(10000);
                if (in >= 0) {
                    if (next < stream.units.size()) {
                        queue(codec, in, stream, next++);
                    } else {
                        codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        sentEnd = true;
                    }
                }
            }
            int out = codec.dequeueOutputBuffer(info, 10000);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                logFormat(name, codec.getOutputFormat());
            } else if (out >= 0) {
                ended = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                boolean render = !ended || info.size > 0;
                codec.releaseOutputBuffer(out, render);
                // One frame at a time: the next waits until this one is drawn and read back.
                if (render && frames.tryAcquire(2, TimeUnit.SECONDS)) rendered++;
            }
        }
        Log.i(TAG, name + ": " + rendered + " frames" + (ended ? ", end of stream" : ""));
        codec.stop();
        codec.release();
        surface.release();
        CountDownLatch down = new CountDownLatch(1);
        gl.post(() -> {
            texture.release();
            EGL14.eglMakeCurrent((EGLDisplay) made[0], EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface((EGLDisplay) made[0], (EGLSurface) made[1]);
            EGL14.eglDestroyContext((EGLDisplay) made[0], (EGLContext) made[2]);
            down.countDown();
        });
        down.await(5, TimeUnit.SECONDS);
        thread.quitSafely();
    }

    private static void decode(String name, Stream stream) throws IOException {
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), null, null, 0);
        codec.start();
        run(name, codec, stream, 0, stream.units.size(), true, 0);
        codec.stop();
        codec.release();
    }

    private static void decodeAhead(String name, Stream stream) throws IOException {
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), null, null, 0);
        codec.start();
        int queued = 0;
        long deadline = System.currentTimeMillis() + 5000;
        while (queued < stream.units.size() && System.currentTimeMillis() < deadline) {
            int in = codec.dequeueInputBuffer(queued == 0 ? 100000 : 50000);
            if (in < 0) break;  // the codec takes no more until frames are taken
            queue(codec, in, stream, queued++);
        }
        run(name, codec, stream, queued, stream.units.size(), true, 0);
        codec.stop();
        codec.release();
    }

    private static void flushed(String name, Stream stream) throws IOException {
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        codec.configure(format(), null, null, 0);
        codec.start();
        run(name + " before", codec, stream, 0, 15, false, 5);
        codec.flush();
        run(name + " after", codec, stream, 15, stream.units.size(), true, 0);
        codec.stop();
        codec.release();
    }

    private static void decodeAsync(String name, Stream stream) throws Exception {
        HandlerThread thread = new HandlerThread("media-codec-test");
        thread.start();
        CountDownLatch ended = new CountDownLatch(1);
        List<String> lines = new ArrayList<>();
        MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
        int[] next = {0};
        codec.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(MediaCodec c, int index) {
                if (next[0] < stream.units.size()) {
                    queue(c, index, stream, next[0]++);
                } else if (next[0] == stream.units.size()) {
                    next[0]++;
                    c.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                }
            }

            @Override
            public void onOutputBufferAvailable(MediaCodec c, int index, MediaCodec.BufferInfo info) {
                if (info.size > 0 || (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) {
                    lines.add(frame(c, index, info));
                }
                c.releaseOutputBuffer(index, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) ended.countDown();
            }

            @Override
            public void onError(MediaCodec c, MediaCodec.CodecException e) {
                lines.add("onError " + e.getClass().getSimpleName());
                ended.countDown();
            }

            @Override
            public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
                lines.add("onOutputFormatChanged " + format.getInteger(MediaFormat.KEY_WIDTH) + " x "
                        + format.getInteger(MediaFormat.KEY_HEIGHT) + crop(format));
            }
        }, new Handler(thread.getLooper()));
        codec.configure(format(), null, null, 0);
        codec.start();
        boolean done = ended.await(10, TimeUnit.SECONDS);
        codec.stop();
        codec.release();
        thread.quitSafely();
        for (String line : lines) {
            Log.i(TAG, name + ": " + line);
        }
        Log.i(TAG, name + ": " + lines.size() + " lines" + (done ? ", end of stream" : ", timed out"));
    }

    private static void refusals() {
        try {
            MediaCodec.createDecoderByType("video/x-minato-unknown").release();
            Log.i(TAG, "an unknown type: a codec");
        } catch (Exception e) {
            Log.i(TAG, "an unknown type: " + e.getClass().getSimpleName());
        }
        try {
            MediaCodec codec = MediaCodec.createByCodecName(sDecoder);
            try {
                codec.dequeueOutputBuffer(new MediaCodec.BufferInfo(), 0);
                Log.i(TAG, "dequeueOutputBuffer before start: no exception");
            } catch (IllegalStateException e) {
                Log.i(TAG, "dequeueOutputBuffer before start: " + e.getClass().getSimpleName());
            }
            codec.release();
        } catch (IOException e) {
            Log.i(TAG, "dequeueOutputBuffer before start: no codec, " + e.getClass().getSimpleName());
        }
    }
}
