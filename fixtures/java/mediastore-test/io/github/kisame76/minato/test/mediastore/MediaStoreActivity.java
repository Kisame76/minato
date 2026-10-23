package io.github.kisame76.minato.test.mediastore;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.util.Log;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;

/**
 * ADR-0057 step e's test: an app without storage permissions inserts an image into MediaStore with IS_PENDING, writes
 * it, finishes it, reads it back and deletes it; inserts a second of the same name, a stream without pending, an
 * audio file and a file in a folder the collection does not allow; and saves a bitmap with insertImage. It logs the
 * URI's form (the id masked), the row's columns, the bytes' SHA-256 and the exceptions. Logs under the tag
 * mediastore-test; tools/android-reference.sh takes the log from Android 15 and tools/mediastore-check.sh compares
 * Minato's with it.
 */
public class MediaStoreActivity extends Activity {
    static final String TAG = "mediastore-test";
    private static final byte[] BYTES = new byte[3000];
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    static {
        for (int i = 0; i < BYTES.length; i++) {
            BYTES[i] = (byte) (i * 7);
        }
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String type(Throwable e) {
        return e.getClass().getName();
    }

    private static String form(Uri uri) {
        return uri == null ? "null" : uri.toString().replaceAll("/[0-9]{1,18}$", "/<id>");
    }

    private static String sha(byte[] bytes, int length) throws Exception {
        StringBuilder text = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(java.util.Arrays.copyOf(bytes, length))) {
            text.append(String.format("%02x", b));
        }
        return text.substring(0, 16);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            try {
                run();
            } catch (Throwable e) {
                log("failed: " + type(e) + ": " + e.getMessage());
            }
            log("done");
        });
    }

    private ContentValues image(String name, String relative, boolean pending) {
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        values.put(MediaStore.MediaColumns.MIME_TYPE, "image/jpeg");
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, relative);
        if (pending) {
            values.put(MediaStore.MediaColumns.IS_PENDING, 1);
        }
        return values;
    }

    private void row(String label, Uri uri) {
        row(label, uri, true);
    }

    /** The row's columns; the size of a row that was not pending is Android's scan, which runs after the close. */
    private void row(String label, Uri uri, boolean withSize) {
        ContentResolver resolver = getContentResolver();
        String[] columns = {MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.TITLE,
            MediaStore.MediaColumns.MIME_TYPE, MediaStore.MediaColumns.RELATIVE_PATH,
            MediaStore.MediaColumns.IS_PENDING, MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.OWNER_PACKAGE_NAME, MediaStore.MediaColumns.VOLUME_NAME,
            MediaStore.MediaColumns.DATE_ADDED, MediaStore.MediaColumns.DATA};
        try (Cursor cursor = resolver.query(uri, columns, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) {
                log(label + ": " + (cursor == null ? "null" : "no row"));
                return;
            }
            File file = new File(cursor.getString(9));
            String name = cursor.getString(0).replaceAll("^[0-9]{13}", "<time>");
            String title = cursor.getString(1).replaceAll("pending-[0-9]+-", "pending-<time>-")
                    .replaceAll("^[0-9]{13}$", "<time>");
            log(label + ": name=" + name + " title=" + title + " type=" + cursor.getString(2) + " path="
                    + cursor.getString(3) + " pending=" + cursor.getInt(4)
                    + (withSize ? " size=" + cursor.getLong(5) : "") + " owner=" + cursor.getString(6) + " volume="
                    + cursor.getString(7) + " added=" + (cursor.getLong(8) > 0) + " file=" + file.isFile()
                    + " filesize=" + file.length());
        } catch (RuntimeException e) {
            log(label + ": " + type(e));
        }
    }

    private void write(String label, Uri uri, int length) throws Exception {
        try (OutputStream out = getContentResolver().openOutputStream(uri, "w")) {
            out.write(BYTES, 0, length);
        }
        log(label + ": wrote " + length);
    }

    private void run() throws Exception {
        ContentResolver resolver = getContentResolver();
        Uri images = MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
        log("collection: " + images);
        log("volumes: " + new java.util.TreeSet<>(MediaStore.getExternalVolumeNames(this)));

        Uri first = resolver.insert(images, image("shot.jpg", "Pictures/Minato", true));
        log("insert: " + form(first));
        row("pending row", first);
        write("pending", first, 2000);
        row("written row", first);
        ContentValues done = new ContentValues();
        done.put(MediaStore.MediaColumns.IS_PENDING, 0);
        log("finish: " + resolver.update(first, done, null, null));
        row("finished row", first);
        try (InputStream in = resolver.openInputStream(first)) {
            byte[] read = in.readAllBytes();
            log("read: " + read.length + " " + sha(read, read.length) + " expected " + sha(BYTES, 2000));
        }
        try (Cursor cursor = resolver.query(images, new String[] {MediaStore.MediaColumns.DISPLAY_NAME},
                MediaStore.MediaColumns.DISPLAY_NAME + " LIKE ?", new String[] {"shot%"}, null)) {
            log("collection query: " + cursor.getCount());
        }

        Uri second = resolver.insert(images, image("shot.jpg", "Pictures/Minato", false));
        log("second insert: " + form(second));
        write("second", second, 500);
        row("second row", second, false);

        ContentValues audio = new ContentValues();
        audio.put(MediaStore.MediaColumns.DISPLAY_NAME, "tune.mp3");
        audio.put(MediaStore.MediaColumns.MIME_TYPE, "audio/mpeg");
        audio.put(MediaStore.MediaColumns.RELATIVE_PATH, "Music/Minato");
        Uri tune = resolver.insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, audio);
        log("audio insert: " + form(tune));
        row("audio row", tune);

        try {
            log("bad folder: " + form(resolver.insert(images, image("doc.jpg", "Documents/x", false))));
        } catch (RuntimeException e) {
            log("bad folder: " + type(e) + ": " + e.getMessage());
        }
        String[][] bad = {{"video", "Music/x"}, {"audio", "Pictures/x"}, {"audio", ""}, {"images", "DCIM/x"}};
        for (String[] probe : bad) {
            Uri collection = probe[0].equals("video") ? MediaStore.Video.Media.EXTERNAL_CONTENT_URI
                    : probe[0].equals("audio") ? MediaStore.Audio.Media.EXTERNAL_CONTENT_URI : images;
            ContentValues values = image("probe.bin", probe[1], false);
            values.put(MediaStore.MediaColumns.MIME_TYPE, probe[0].equals("audio") ? "audio/mpeg"
                    : probe[0].equals("video") ? "video/mp4" : "image/jpeg");
            try {
                Uri made = resolver.insert(collection, values);
                log("folder " + probe[0] + " " + probe[1] + ": " + form(made));
                row("folder row", made, false);
                resolver.delete(made, null, null);
            } catch (RuntimeException e) {
                log("folder " + probe[0] + " " + probe[1] + ": " + type(e) + ": " + e.getMessage());
            }
        }
        try {
            ContentValues noName = new ContentValues();
            noName.put(MediaStore.MediaColumns.MIME_TYPE, "image/jpeg");
            Uri unnamed = resolver.insert(images, noName);
            log("no name: " + form(unnamed));
            row("no name row", unnamed);
            log("no name delete: " + resolver.delete(unnamed, null, null));
        } catch (RuntimeException e) {
            log("no name: " + type(e));
        }

        Bitmap bitmap = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        String saved = MediaStore.Images.Media.insertImage(resolver, bitmap, "legacy", "from insertImage");
        log("insertImage: " + (saved == null ? "null" : form(Uri.parse(saved))));
        if (saved != null) {
            row("legacy row", Uri.parse(saved));
            log("legacy delete: " + resolver.delete(Uri.parse(saved), null, null));
        }

        log("delete first: " + resolver.delete(first, null, null));
        row("deleted row", first);
        log("delete audio: " + resolver.delete(tune, null, null));
        Thread.sleep(3000);
        try (Cursor cursor = resolver.query(second, new String[] {MediaStore.MediaColumns.SIZE}, null, null, null)) {
            cursor.moveToFirst();
            log("second size later: " + cursor.getLong(0));
        }
        log("delete second: " + resolver.delete(second, null, null));
        try {
            log("delete again: " + resolver.delete(first, null, null));
        } catch (RuntimeException e) {
            log("delete again: " + type(e));
        }
    }
}
