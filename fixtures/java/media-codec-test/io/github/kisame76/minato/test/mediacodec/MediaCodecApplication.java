package io.github.kisame76.minato.test.mediacodec;

import android.app.Application;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

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
 *   window through AMediaCodec, each image's time and samples logged as the buffers' are.
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
            logNative(decodeToReaderNative(sDecoder, main.bytes, flatten(main)));
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
