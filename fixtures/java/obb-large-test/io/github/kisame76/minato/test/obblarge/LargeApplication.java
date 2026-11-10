package io.github.kisame76.minato.test.obblarge;

import android.app.Application;
import android.content.Context;
import android.os.StatFs;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;

/**
 * An app reads a main OBB of more than 2 GiB (tools/make-sparse-obb.py: tags at the start, spanning 2^31, above 2^31
 * and at the end) through the calls a game uses: File.length, RandomAccessFile, FileInputStream.skip, a positional
 * FileChannel read and a mapping that starts below 2^31 and reaches past it. Each line shows the tag it found.
 */
public class LargeApplication extends Application {
    private static final String TAG = "obb-large-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.obblarge";
    private static final long GIB2 = 1L << 31;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private static String text(byte[] bytes) {
        return new String(bytes, StandardCharsets.UTF_8);
    }

    static void report(Context base) throws IOException {
        final File obb = new File(base.getObbDir(), "main.1." + PACKAGE + ".obb");
        final long length = obb.length();
        Log.i(TAG, "size " + length + " (" + (length - GIB2) + " above 2 GiB)");
        try (RandomAccessFile file = new RandomAccessFile(obb, "r")) {
            final byte[] tag = new byte[8];
            file.seek(0);
            file.readFully(tag);
            Log.i(TAG, "start: " + text(tag));
            file.seek(GIB2 - 4);
            file.readFully(tag);
            Log.i(TAG, "spanning 2 GiB: " + text(tag));
            file.seek(length - 8);
            file.readFully(tag);
            Log.i(TAG, "end: " + text(tag) + ", pointer " + (file.getFilePointer() == length));
        }
        try (FileInputStream in = new FileInputStream(obb)) {
            long skipped = 0;
            while (skipped < GIB2 + 4096) {
                final long step = in.skip(GIB2 + 4096 - skipped);
                if (step <= 0) {
                    break;
                }
                skipped += step;
            }
            final byte[] tag = in.readNBytes(8);
            Log.i(TAG, "stream skipped " + skipped + ": " + text(tag));
        }
        try (FileChannel channel = FileChannel.open(obb.toPath(), java.nio.file.StandardOpenOption.READ)) {
            final ByteBuffer buffer = ByteBuffer.allocate(8);
            channel.read(buffer, GIB2 + 4096);
            Log.i(TAG, "channel read above 2 GiB: " + text(buffer.array()));
            final MappedByteBuffer map = channel.map(FileChannel.MapMode.READ_ONLY, GIB2 - 4096, 8192);
            final byte[] tag = new byte[8];
            map.position(4096 - 4);
            map.get(tag);
            Log.i(TAG, "mapped across 2 GiB: " + text(tag));
        }
        try (FileChannel channel = FileChannel.open(obb.toPath(), java.nio.file.StandardOpenOption.READ)) {
            final MappedByteBuffer map = channel.map(FileChannel.MapMode.READ_ONLY, GIB2, 8192);
            final byte[] tag = new byte[8];
            map.position(4096);
            map.get(tag);
            Log.i(TAG, "mapped from 2 GiB: " + text(tag));
        }
        Log.i(TAG, "free space known: " + (new StatFs(base.getObbDir().getPath()).getAvailableBytes() > 0));
    }
}
