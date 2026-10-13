package io.github.kisame76.minato.test.isolated.feature;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

/** A provider of the feature split, installed at the start: it logs which class loader made it. */
public class FeatureProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        ClassLoader mine = getClass().getClassLoader();
        ClassLoader apps = getContext().getClassLoader();
        Log.i("isolated-splits-test", "feature provider made by the app's loader: " + (mine == apps));
        return true;
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
