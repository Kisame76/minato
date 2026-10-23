package io.github.kisame76.minato.test.openstart;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;

/** Two files the app shares: content://.../report.txt and content://.../a b.png (a name with a space and a slash-less look). */
public class SharedFiles extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    private File file(Uri uri) throws FileNotFoundException {
        final String name = uri.getLastPathSegment();
        final File file = new File(getContext().getCacheDir(), "shared-" + (name.equals("report.txt") ? "report" : "image"));
        if (!file.exists()) {
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write((name.equals("report.txt") ? "the report\n" : "PNG?").getBytes());
            } catch (IOException e) {
                throw new FileNotFoundException(e.toString());
            }
        }
        return file;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        final MatrixCursor cursor = new MatrixCursor(new String[] {"_display_name"});
        cursor.addRow(new Object[] {uri.getLastPathSegment().equals("report.txt") ? "report.txt" : "a b/c.png"});
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        return uri.getLastPathSegment().equals("report.txt") ? "text/plain" : "image/png";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
