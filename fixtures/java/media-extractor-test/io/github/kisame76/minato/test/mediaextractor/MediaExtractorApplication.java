package io.github.kisame76.minato.test.mediaextractor;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * ADR-0060 e2d's test of the NDK's AMediaExtractor, on Minato's own mp4 files (tools/make-mp4-sample.sh: baseline.mp4
 * with an AAC track and main.mp4 with B-frames, in its assets). On a thread that Application.attachBaseContext starts,
 * for each file, the JNI library opens it from the APK's asset as a file descriptor with an offset and a length, from a
 * copy in the app's files by its path, and from its bytes through an AMediaDataSource, and logs its tracks' formats,
 * every sample, seeks
 * in each mode, the refusals and files it cannot take (jni/media_extractor_test.c says which); then
 * android.media.MediaExtractor does the same from Java (ADR-0060 e2d part 2b, JavaExtractor says which). It logs with
 * the tag media-extractor-test and ends with "done".
 */
public class MediaExtractorApplication extends Application {
    private static final String TAG = "media-extractor-test";

    static {
        System.loadLibrary("media-extractor-test");
    }

    /** One file through the three sources; the log lines, joined by newlines. */
    private static native String extract(String name, int fd, long offset, long length, String path, byte[] bytes);

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        // On a thread of its own: the Java half's sources take longer than Android lets an app's start take.
        new Thread(() -> test(base), "media-extractor-test").start();
    }

    private static void test(Context base) {
        try {
            for (String name : new String[] {"baseline", "main"}) {
                try (AssetFileDescriptor asset = base.getAssets().openFd(name + ".mp4")) {
                    byte[] bytes = read(base, name + ".mp4");
                    File copy = new File(base.getFilesDir(), name + ".mp4");
                    try (FileOutputStream out = new FileOutputStream(copy)) {
                        out.write(bytes);
                    }
                    logNative(extract(name, asset.getParcelFileDescriptor().getFd(), asset.getStartOffset(),
                            asset.getLength(), copy.getPath(), bytes));
                    logNative(JavaExtractor.extract(base, name, asset, copy, bytes));
                }
            }
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private static byte[] read(Context context, String asset) throws IOException {
        try (InputStream in = context.getAssets().open(asset)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = in.read(chunk)) > 0;) out.write(chunk, 0, count);
            return out.toByteArray();
        }
    }

    /** The JNI part's lines, those that start with "device " to the device tag. */
    private static void logNative(String lines) {
        for (String line : lines.split("\n")) {
            if (line.isEmpty()) continue;
            if (line.startsWith("device ")) {
                Log.i(TAG + "-device", line.substring(7));
            } else {
                Log.i(TAG, line);
            }
        }
    }
}
