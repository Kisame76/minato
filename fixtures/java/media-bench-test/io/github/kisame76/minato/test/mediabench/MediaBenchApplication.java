package io.github.kisame76.minato.test.mediabench;

import android.app.Application;
import android.content.Context;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * ADR-0060 e2c's iPhone measurement: Minato's own H.264 streams at 1080p (hd) and 4K (uhd), 60 frames each of Main with
 * B-frames, decoded through the NDK's AMediaCodec in the app's JNI library with the first video/avc decoder that is
 * not the emulator's goldfish one, from Application.attachBaseContext:
 * - each stream into ByteBuffers, into a YUV_420_888 AImageReader (VTPixelTransferSession on Minato) and into an
 *   RGBA_8888 one (Minato's Metal pass), after one round to warm up three rounds each: the frames per second from the
 *   first unit to the end of the stream, and the time releaseOutputBuffer takes (p50 and p99), which on Minato is the
 *   host's copy into the reader's buffer;
 * - uhd decoded again and again for 30 s into an RGBA and then a YUV reader while the check sends the app to the
 *   background and back (tools/java-start-check.sh --device): the frames, the images, the codec's errors, the longest
 *   time without an image and whether images come again after the return.
 * A measurement of Minato's: no Android reference. It logs with the tag media-bench-test and ends with "done".
 */
public class MediaBenchApplication extends Application {
    private static final String TAG = "media-bench-test";

    static {
        System.loadLibrary("media-bench-test");
    }

    /** Rounds of the stream with the named decoder into `output` (0 ByteBuffers, 1 YUV reader, 2 RGBA reader). */
    private static native String benchNative(String decoder, String name, byte[] stream, long[] index, int width,
            int height, int output);

    /** The stream again and again for `seconds` into a reader (1 YUV, 2 RGBA), across the background. */
    private static native String backgroundNative(String decoder, byte[] stream, long[] index, int width, int height,
            int output, int seconds);

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            String decoder = decoder();
            Log.i(TAG, "decoder: " + decoder);
            String[] names = {"hd", "uhd"};
            int[][] sizes = {{1920, 1080}, {3840, 2160}};
            byte[][] streams = new byte[2][];
            long[][] indexes = new long[2][];
            for (int s = 0; s < 2; s++) {
                streams[s] = read(base.getAssets().open(names[s] + ".h264"));
                indexes[s] = index(read(base.getAssets().open(names[s] + ".idx")));
                for (int output = 0; output < 3; output++) {
                    log(benchNative(decoder, names[s], streams[s], indexes[s], sizes[s][0], sizes[s][1], output));
                }
            }
            Log.i(TAG, "background rgba: decoding");
            log(backgroundNative(decoder, streams[1], indexes[1], 3840, 2160, 2, 30));
            Log.i(TAG, "background yuv: decoding");
            log(backgroundNative(decoder, streams[1], indexes[1], 3840, 2160, 1, 30));
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    private static void log(String lines) {
        for (String line : lines.split("\n")) {
            if (!line.isEmpty()) Log.i(TAG, line);
        }
    }

    private static String decoder() {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || info.getName().contains(".goldfish.")) continue;
            for (String type : info.getSupportedTypes()) {
                if (type.equalsIgnoreCase(MediaFormat.MIMETYPE_VIDEO_AVC)) return info.getName();
            }
        }
        return null;
    }

    /** The index's lines, flattened: offset, size, time in microseconds and key flag of each access unit. */
    private static long[] index(byte[] text) throws IOException {
        String[] lines = new String(text, "UTF-8").split("\n");
        long[] all = new long[lines.length * 4];
        int count = 0;
        for (String line : lines) {
            if (line.isEmpty()) continue;
            for (String part : line.split(" ")) all[count++] = Long.parseLong(part);
        }
        long[] exact = new long[count];
        System.arraycopy(all, 0, exact, 0, count);
        return exact;
    }

    private static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] chunk = new byte[65536];
        for (int n; (n = in.read(chunk)) > 0; ) {
            out.write(chunk, 0, n);
        }
        in.close();
        return out.toByteArray();
    }
}
