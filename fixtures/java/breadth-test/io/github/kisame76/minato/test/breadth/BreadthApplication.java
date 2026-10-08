package io.github.kisame76.minato.test.breadth;

import android.app.Application;
import android.os.Debug;
import android.util.Base64;
import android.util.Base64DataException;
import android.util.Base64InputStream;
import android.util.Base64OutputStream;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;

/**
 * The framework breadth's behavior test, batch by batch, without a window. It logs under the tag breadth-test what the
 * batches' types answer. Batch 3a, the Unity test APK probe's types:
 * <ul>
 * <li>debug: what Crashlytics asks before it reports a crash (isDebuggerConnected and waitingForDebugger), with no
 * debugger attached, and an empty MemoryInfo;
 * <li>base64: Base64OutputStream and Base64InputStream with Base64's flags, written in pieces, a long input's lines,
 * the URL-safe, unwrapped and unpadded chain over GZIP that Firebase's heartbeat header writes, and a bad input's
 * exception, after a character outside the alphabet that it skips.
 * </ul>
 * A failing part logs "failed" and its stack under breadth-test-device, which differs by device and is only shown.
 * tools/android-reference.sh takes the log from Android 15, and tools/breadth-check.sh compares Minato's with it.
 */
public class BreadthApplication extends Application {
    private static final String TAG = "breadth-test";

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("debug", BreadthApplication::debug);
        part("base64", BreadthApplication::base64);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** What Crashlytics asks before it reports a crash, and an empty MemoryInfo. */
    private static void debug() {
        Log.i(TAG, "debug: debugger connected " + Debug.isDebuggerConnected() + ", waiting for one "
                + Debug.waitingForDebugger());
        Debug.MemoryInfo info = new Debug.MemoryInfo();
        Log.i(TAG, "debug: an empty MemoryInfo's total PSS " + info.getTotalPss() + ", its summary's "
                + info.getMemoryStat("summary.total-pss") + ", its Java heap's "
                + info.getMemoryStat("summary.java-heap"));
        // The thread's CPU time differs by device and run, so it is only shown.
        Log.i(TAG + "-device", "debug: thread CPU time " + Debug.threadCpuTimeNanos() + " ns");
    }

    /** The Base64 streams with each of Base64's flags, a long input and Firebase's chain over GZIP. */
    private static void base64() throws IOException {
        byte[] data = "Minato, the harbor: \u6e2f".getBytes(StandardCharsets.UTF_8);
        int[] flags = {Base64.DEFAULT, Base64.NO_WRAP, Base64.NO_PADDING | Base64.NO_WRAP,
                Base64.URL_SAFE | Base64.NO_WRAP, Base64.CRLF};
        for (int flag : flags) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (OutputStream out = new Base64OutputStream(bytes, flag)) {
                // In pieces, so the stream carries the bytes that make no whole group.
                out.write(data, 0, 5);
                out.write(data[5]);
                out.write(data, 6, data.length - 6);
            }
            byte[] encoded = bytes.toByteArray();
            byte[] back = readAll(new Base64InputStream(new ByteArrayInputStream(encoded), flag));
            Log.i(TAG, "base64: flags 0x" + Integer.toHexString(flag) + ": " + shown(encoded) + ", back equal "
                    + Arrays.equals(data, back) + ", same as Base64.encode " + Arrays.equals(encoded,
                    Base64.encode(data, flag)));
        }
        byte[] counting = new byte[100];
        for (int i = 0; i < counting.length; i++) {
            counting[i] = (byte) i;
        }
        ByteArrayOutputStream lines = new ByteArrayOutputStream();
        try (OutputStream out = new Base64OutputStream(lines, Base64.DEFAULT)) {
            out.write(counting);
        }
        Log.i(TAG, "base64: 100 bytes: " + shown(lines.toByteArray()));
        int firebase = Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING;
        ByteArrayOutputStream header = new ByteArrayOutputStream();
        try (OutputStream out = new GZIPOutputStream(new Base64OutputStream(header, firebase))) {
            out.write("{\"heartbeats\":[{\"agent\":\"fire-core/20.3.0\",\"dates\":[\"2026-10-08\"]}]}"
                    .getBytes(StandardCharsets.UTF_8));
        }
        String text = header.toString("US-ASCII");
        // GZIP's deflated bytes may differ by zlib, so only the alphabet and the way back are compared.
        byte[] json = readAll(new GZIPInputStream(new Base64InputStream(new ByteArrayInputStream(header.toByteArray()),
                firebase)));
        Log.i(TAG, "base64: Firebase's header URL-safe " + text.matches("[A-Za-z0-9_-]+") + ", back "
                + new String(json, StandardCharsets.UTF_8));
        byte[] skipped = readAll(new Base64InputStream(
                new ByteArrayInputStream("TW!lu".getBytes(StandardCharsets.US_ASCII)), Base64.DEFAULT));
        Log.i(TAG, "base64: a character outside the alphabet skipped: " + new String(skipped, StandardCharsets.UTF_8));
        try {
            // A character outside the alphabet is skipped, as Base64 does; padding first is not.
            readAll(new Base64InputStream(new ByteArrayInputStream("=abc".getBytes(StandardCharsets.US_ASCII)),
                    Base64.DEFAULT));
            Log.i(TAG, "base64: a bad input read");
        } catch (Base64DataException e) {
            Log.i(TAG, "base64: a bad input: " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream stream = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[64];
            for (int count; (count = stream.read(buffer)) > 0; ) {
                out.write(buffer, 0, count);
            }
            return out.toByteArray();
        }
    }

    /** Encoded bytes as text, with line ends shown. */
    private static String shown(byte[] encoded) {
        return new String(encoded, StandardCharsets.US_ASCII).replace("\r", "\\r").replace("\n", "\\n");
    }
}
