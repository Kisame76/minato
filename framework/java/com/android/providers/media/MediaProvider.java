/*
 * The media provider (ADR-0057 step c): Android's com.android.providers.media.MediaProvider, local in the app's
 * process, which ActivityThread.acquireProvider gives for the authority "media". It serves the photo picker's URIs,
 * content://media/picker/0/com.android.providers.media.photopicker/media/<ID>, of the photos and videos the system
 * picker returned [verified: photos-test on apk-runner-35]:
 *   - the app reads them with the grant the picker made (read, and it may keep it); without one, a query and an open
 *     throw a SecurityException; a write mode throws one too;
 *   - a query without a projection answers the columns duration, orientation, height, _display_name, datetaken,
 *     mime_type, _data, _size, width, in this order; the display name is "<ID>.<extension>"; the type of the URI is
 *     the clone's MIME type.
 * Departures: _data is the clone's real path in the app's data folder, where Android's is a synthetic path below
 * /sdcard/.transforms that the app's file access reaches through its FUSE daemon (the real path opens; the synthetic
 * one does not here); a column that the picker does not have is refused with an IllegalArgumentException, which
 * Android's behavior for it is not measured. Every other URI of "media" is the app's own media (step e), MediaLibrary;
 * a call is an IllegalArgumentException. Not under com.android.minato, whose classes tools/framework-host-check.sh
 * compiles without android.*.
 */
package com.android.providers.media;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.content.Intent;

import com.android.internal.minato.picker.UriGrants;
import com.android.minato.picker.PhotoManifest;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

/** @hide */
public final class MediaProvider extends ContentProvider {
    /** The columns of a picked photo, in the order a query without a projection answers them. */
    private static final String[] ALL_COLUMNS = {"duration", "orientation", "height", "_display_name", "datetaken",
        "mime_type", "_data", "_size", "width"};

    private MediaLibrary mLibrary;

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The app's own media: the rows its inserts made (step e). */
    private synchronized MediaLibrary library() {
        if (mLibrary == null) {
            mLibrary = new MediaLibrary(getContext());
        }
        return mLibrary;
    }

    /** The photo's ID when `uri` is a picker URI of the photo picker, else -1. */
    private static long pickerId(Uri uri) {
        final List<String> segments = uri.getPathSegments();
        if (segments.size() != 5 || !segments.get(0).equals("picker") || !segments.get(1).equals("0")
                || !segments.get(2).equals(UriGrants.PHOTO_AUTHORITY) || !segments.get(3).equals("media")
                || !segments.get(4).matches("[0-9]{1,18}")) {
            return -1;
        }
        return Long.parseLong(segments.get(4));
    }

    private void enforceRead(Uri uri) {
        if (!UriGrants.allows(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) {
            throw new SecurityException("Permission Denial: reading " + MediaProvider.class.getName() + " uri " + uri
                    + " from pid=" + Binder.getCallingPid() + ", uid=" + Binder.getCallingUid()
                    + " requires android.permission.READ_EXTERNAL_STORAGE, or grantUriPermission()");
        }
    }

    private static File fileOf(PhotoManifest.Item item) {
        return new File(UriGrants.photoFolder(), item.name);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        if (pickerId(uri) < 0) {
            return library().query(uri, projection, selection, selectionArgs, sortOrder);
        }
        enforceRead(uri);
        final String[] columns = projection != null ? projection : ALL_COLUMNS;
        for (final String column : columns) {
            if (!column.equals("duration_millis") && !java.util.Arrays.asList(ALL_COLUMNS).contains(column)) {
                throw new IllegalArgumentException("Invalid column " + column);
            }
        }
        final MatrixCursor result = new MatrixCursor(columns);
        final PhotoManifest.Item item = UriGrants.photos().get(pickerId(uri));
        if (item != null) {
            final MatrixCursor.RowBuilder row = result.newRow();
            for (final String column : columns) {
                row.add(column, value(uri, item, column));
            }
        }
        return result;
    }

    private static Object value(Uri uri, PhotoManifest.Item item, String column) {
        switch (column) {
            case "duration":
            case "duration_millis":
                return item.durationMs;
            case "orientation":
                return item.orientation;
            case "height":
                return item.height;
            case "width":
                return item.width;
            case "_display_name":
                return pickerId(uri) + item.name.substring(item.name.lastIndexOf('.'));
            case "datetaken":
                return item.takenMs;
            case "mime_type":
                return item.mimeType();
            case "_data":
                return fileOf(item).getPath();
            default:
                return fileOf(item).length();
        }
    }

    @Override
    public String getType(Uri uri) {
        final long id = pickerId(uri);
        if (id < 0) {
            return library().getType(uri);
        }
        final PhotoManifest.Item item = UriGrants.photos().get(id);
        return item == null ? null : item.mimeType();
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        final long id = pickerId(uri);
        if (id < 0) {
            return library().open(uri, mode);
        }
        enforceRead(uri);
        if (!mode.equals("r")) {
            throw new SecurityException("Permission Denial: writing " + MediaProvider.class.getName() + " uri " + uri
                    + " from pid=" + Binder.getCallingPid() + ", uid=" + Binder.getCallingUid()
                    + " requires android.permission.WRITE_EXTERNAL_STORAGE, or grantUriPermission()");
        }
        final PhotoManifest.Item item = UriGrants.photos().get(id);
        if (item == null) {
            throw new FileNotFoundException("No photo for " + uri);
        }
        return ParcelFileDescriptor.open(fileOf(item), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return library().insert(uri, values);
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return library().update(uri, values, selection, selectionArgs);
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return library().delete(uri, selection, selectionArgs);
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        throw new IllegalArgumentException("Unknown URI content://media");
    }
}
