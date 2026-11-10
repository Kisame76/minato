package io.github.kisame76.minato.test.obbzip64;

import android.app.Application;
import android.content.Context;
import android.util.Log;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * An app opens its main OBB as a Zip64 archive (tools/make-sparse-zip64-obb.py: a stored entry of 4 GiB and 64 KiB and a
 * small one behind it, whose offset is beyond 2^32) with java.util.zip.ZipFile and reads both entries; it logs what the
 * entries say and the bytes it reads, or the exception Android's ZipFile throws, which the log then holds as well.
 */
public class ZipApplication extends Application {
    private static final String TAG = "obb-zip64-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.obbzip64";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t.getClass().getName());
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private static String text(byte[] bytes) {
        return new String(bytes, StandardCharsets.UTF_8);
    }

    static void report(Context base) throws IOException {
        final File obb = new File(base.getObbDir(), "main.1." + PACKAGE + ".obb");
        Log.i(TAG, "size " + obb.length());
        try (ZipFile zip = new ZipFile(obb)) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                Log.i(TAG, "entry " + entry.getName() + " size " + entry.getSize() + " compressed "
                        + entry.getCompressedSize() + " method " + entry.getMethod());
            }
            final ZipEntry after = zip.getEntry("after.txt");
            try (InputStream in = zip.getInputStream(after)) {
                Log.i(TAG, "after.txt: " + text(in.readAllBytes()).trim());
            }
            final ZipEntry big = zip.getEntry("big.bin");
            try (InputStream in = zip.getInputStream(big)) {
                long skipped = 0;
                while (skipped < big.getSize() - 8) {
                    final long step = in.skip(big.getSize() - 8 - skipped);
                    if (step <= 0) {
                        break;
                    }
                    skipped += step;
                }
                Log.i(TAG, "big.bin skipped " + skipped + ", tail: " + text(in.readNBytes(8)));
            }
        }
    }
}
