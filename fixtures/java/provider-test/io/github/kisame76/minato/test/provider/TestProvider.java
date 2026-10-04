package io.github.kisame76.minato.test.provider;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

/**
 * The test's providers: each logs what installProvider hands its attachInfo (the ProviderInfo, without meta-data, and
 * the Application as its context) and its onCreate, which attachInfo calls. They answer no query: calls into them
 * come with step c2d's ContentResolver.
 */
public abstract class TestProvider extends ContentProvider {
    private String name() {
        return getClass().getSimpleName();
    }

    @Override
    public void attachInfo(Context context, ProviderInfo info) {
        Log.i(ProviderApplication.TAG, name() + " attachInfo authority " + info.authority + ", initOrder "
                + info.initOrder + ", exported " + info.exported + ", process " + info.processName + ", meta-data "
                + (info.metaData == null ? "none" : info.metaData.keySet()) + ", context is the application "
                + (context == ProviderApplication.sApplication));
        super.attachInfo(context, info);
        Log.i(ProviderApplication.TAG, name() + " attached, getContext is the application "
                + (getContext() == ProviderApplication.sApplication));
    }

    @Override
    public boolean onCreate() {
        Log.i(ProviderApplication.TAG, name() + " onCreate");
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
