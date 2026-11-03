package io.github.kisame76.minato.test.trimmemory;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;

/** The provider of trim-memory-test: it holds no data and logs the memory callbacks it gets. */
public class TrimProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        TrimApplication.memory("provider onTrimMemory " + level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        TrimApplication.memory("provider onLowMemory");
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
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
