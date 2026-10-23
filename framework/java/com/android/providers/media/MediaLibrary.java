/*
 * The app's own media (ADR-0057 step e), which Android's MediaProvider keeps for an app that holds no storage
 * permission: the images, videos and audio files the app inserts into MediaStore, with their rows. A database in the
 * app's data folder, <data>/.minato/library/media.db, holds the rows and the files are below
 * <data>/.minato/library/files/<RELATIVE_PATH>; _data is the file's real path. The rules of the names and folders
 * are com.android.minato.media.MediaRules, measured on Android 15 by mediastore-test. An item is created with
 * IS_PENDING (its file is ".pending-<expiry>-<name>" until IS_PENDING is 0), and an image or a video that is
 * finished, or whose stream closes for an insert that was not pending, is exported to Photos (decision 5).
 * Departures: the files are not below /storage/emulated/0, as the app has no such folder to share; other apps' media
 * are not there, as without READ_MEDIA_IMAGES; thumbnails, scans, trash and favorites are not done.
 */
package com.android.providers.media;

import android.app.ActivityThread;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.FileUtils;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelFileDescriptor;
import android.system.OsConstants;
import android.util.Log;

import com.android.internal.minato.picker.UriGrants;
import com.android.minato.media.MediaRules;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** @hide */
final class MediaLibrary {
    private static final String TAG = "MediaLibrary";
    private static final long PENDING_SECONDS = 7 * 24 * 3600;
    private static final String VOLUME = "external_primary";
    /** The columns an insert or an update may set; the others are the provider's. */
    private static final Set<String> SETTABLE = Set.of("title", "description", "datetaken", "width", "height",
            "orientation", "duration", "is_favorite", "date_modified");

    private final Context mContext;
    private SQLiteDatabase mDatabase;
    private HandlerThread mThread;
    /** The items a stream of the app writes to, whose size a query takes from the file: Android's scan runs soon. */
    private final Set<Long> mWriting = java.util.concurrent.ConcurrentHashMap.newKeySet();

    MediaLibrary(Context context) {
        mContext = context;
    }

    /** A parsed collection URI: its volume, collection (images, video, audio or file), and the item's ID or -1. */
    private static final class Target {
        String volume;
        String collection;
        long id = -1;
    }

    private static Target parse(Uri uri) {
        final List<String> segments = uri.getPathSegments();
        if (segments.size() < 3 || segments.size() > 4 || !segments.get(2).equals("media")
                && !segments.get(1).equals("file") || !(segments.get(0).equals("external")
                || segments.get(0).equals(VOLUME))) {
            return null;
        }
        final Target target = new Target();
        target.volume = segments.get(0);
        target.collection = segments.get(1);
        if (!List.of("images", "video", "audio", "file").contains(target.collection)) {
            return null;
        }
        final String last = segments.get(segments.size() - 1);
        if (segments.size() == 4 || segments.size() == 3 && target.collection.equals("file")) {
            if (!last.matches("[0-9]{1,18}")) {
                return null;
            }
            target.id = Long.parseLong(last);
        }
        return target;
    }

    private static Target require(Uri uri) {
        final Target target = parse(uri);
        if (target == null) {
            throw new IllegalArgumentException("Unknown URL " + uri);
        }
        return target;
    }

    private File root() {
        final File folder = UriGrants.folder();
        if (folder == null) {
            throw new IllegalStateException("No application");
        }
        return new File(folder, "library");
    }

    private synchronized SQLiteDatabase database() {
        if (mDatabase == null) {
            final File root = root();
            root.mkdirs();
            final SQLiteDatabase database = SQLiteDatabase.openOrCreateDatabase(new File(root, "media.db"), null);
            database.execSQL("CREATE TABLE IF NOT EXISTS files (_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    + "_data TEXT UNIQUE, _display_name TEXT, title TEXT, mime_type TEXT, relative_path TEXT, "
                    + "is_pending INTEGER DEFAULT 0, _size INTEGER DEFAULT 0, date_added INTEGER, "
                    + "date_modified INTEGER, date_expires INTEGER, owner_package_name TEXT, volume_name TEXT, "
                    + "media_type INTEGER, description TEXT, width INTEGER, height INTEGER, duration INTEGER, "
                    + "datetaken INTEGER, orientation INTEGER, bucket_id INTEGER, bucket_display_name TEXT, "
                    + "is_trashed INTEGER DEFAULT 0, is_favorite INTEGER DEFAULT 0)");
            // Android's IDs of the files of a fresh volume are around 1000000050.
            database.execSQL("INSERT OR IGNORE INTO sqlite_sequence (name, seq) VALUES ('files', 1000000050)");
            mDatabase = database;
        }
        return mDatabase;
    }

    private Handler handler() {
        if (mThread == null) {
            mThread = new HandlerThread("MediaLibrary");
            mThread.start();
        }
        return new Handler(mThread.getLooper());
    }

    private static String where(Target target, String selection) {
        final StringBuilder text = new StringBuilder("1 = 1");
        if (target.id >= 0) {
            text.append(" AND _id = ").append(target.id);
        } else if (!target.collection.equals("file")) {
            text.append(" AND media_type = ").append(MediaRules.mediaType(target.collection))
                    .append(" AND is_pending = 0 AND is_trashed = 0");
        } else {
            text.append(" AND is_pending = 0 AND is_trashed = 0");
        }
        return selection == null || selection.isEmpty() ? text.toString() : text + " AND (" + selection + ")";
    }

    String getType(Uri uri) {
        final Target target = parse(uri);
        if (target == null) {
            return null;
        }
        if (target.id < 0) {
            return target.collection.equals("file") ? "vnd.android.cursor.dir/file"
                    : "vnd.android.cursor.dir/" + (target.collection.equals("images") ? "image" : target.collection);
        }
        try (Cursor cursor = database().query("files", new String[] {"mime_type"}, "_id = ?",
                new String[] {Long.toString(target.id)}, null, null, null)) {
            return cursor.moveToFirst() ? cursor.getString(0) : null;
        }
    }

    Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        final Target target = require(uri);
        refreshSizes();
        return database().query("files", projection, where(target, selection), selectionArgs, null, null, sortOrder);
    }

    private File fileOf(String data) {
        return new File(data);
    }

    Uri insert(Uri uri, ContentValues values) {
        final Target target = require(uri);
        if (target.id >= 0 || target.collection.equals("file")) {
            throw new IllegalArgumentException("Unknown URL " + uri);
        }
        final String relative = MediaRules.relativePath(target.collection, target.volume,
                values.getAsString("relative_path"));
        String mime = values.getAsString("mime_type");
        String name = values.getAsString("_display_name");
        if (name != null) {
            name = FileUtils.buildValidFatFilename(name);
        }
        if (name == null || name.isEmpty()) {
            final String title = values.getAsString("title");
            name = title != null && !title.isEmpty() ? FileUtils.buildValidFatFilename(title)
                    : MediaRules.unnamed(System.currentTimeMillis(), mime);
        }
        if (mime == null) {
            mime = MediaRules.mimeOf(name);
        }
        name = MediaRules.fileName(name, mime);
        final boolean pending = values.getAsInteger("is_pending") != null && values.getAsInteger("is_pending") != 0;
        final long now = System.currentTimeMillis() / 1000;
        final long expires = now + PENDING_SECONDS;
        final SQLiteDatabase database = database();
        final Set<String> taken = new HashSet<>();
        try (Cursor cursor = database.query("files", new String[] {"_display_name"}, "relative_path = ?",
                new String[] {relative}, null, null, null)) {
            while (cursor.moveToNext()) {
                taken.add(cursor.getString(0));
            }
        }
        name = MediaRules.unique(name, taken);
        final String given = values.getAsString("title");
        final String title = given != null && !given.isEmpty() && values.getAsString("_display_name") == null
                ? given : MediaRules.title(name);
        final File file = new File(new File(root(), "files/" + relative),
                pending ? MediaRules.pendingName(name, expires) : name);
        final ContentValues row = new ContentValues();
        for (final String column : SETTABLE) {
            if (values.containsKey(column)) {
                row.put(column, values.getAsString(column));
            }
        }
        row.put("_data", file.getPath());
        row.put("_display_name", name);
        row.put("title", pending ? MediaRules.pendingTitle(title, expires) : title);
        row.put("mime_type", mime);
        row.put("relative_path", relative);
        row.put("is_pending", pending ? 1 : 0);
        row.put("_size", 0);
        row.put("date_added", now);
        row.put("date_modified", now);
        row.put("date_expires", pending ? expires : null);
        row.put("owner_package_name", mContext.getPackageName());
        row.put("volume_name", VOLUME);
        row.put("media_type", MediaRules.mediaType(target.collection));
        final String bucket = relative.substring(0, relative.length() - 1);
        row.put("bucket_display_name", bucket.substring(bucket.lastIndexOf('/') + 1));
        row.put("bucket_id", bucket.toLowerCase(Locale.ROOT).hashCode());
        final long id = database.insert("files", null, row);
        if (id < 0) {
            throw new IllegalStateException("Cannot insert into " + uri);
        }
        return ContentUris.withAppendedId(base(uri, target), id);
    }

    private static Uri base(Uri uri, Target target) {
        return Uri.parse("content://media/" + target.volume + "/" + target.collection + "/media");
    }

    ParcelFileDescriptor open(Uri uri, String mode) throws FileNotFoundException {
        final Target target = require(uri);
        final ContentValues row = row(target.id);
        if (row == null) {
            throw new FileNotFoundException("No item at " + uri);
        }
        final File file = fileOf(row.getAsString("_data"));
        final int bits = ParcelFileDescriptor.parseMode(mode);
        final boolean writing = (bits & ParcelFileDescriptor.MODE_WRITE_ONLY) != 0;
        if (!writing) {
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        file.getParentFile().mkdirs();
        mWriting.add(target.id);
        try {
            return ParcelFileDescriptor.open(file, bits | ParcelFileDescriptor.MODE_CREATE, handler(),
                    e -> finished(target.id, target.collection));
        } catch (IOException e) {
            mWriting.remove(target.id);
            throw new FileNotFoundException("Cannot open " + uri + ": " + e);
        }
    }

    /** The rows of the items being written take their size from their files, as the scan after a close would. */
    private void refreshSizes() {
        for (final long id : mWriting) {
            final ContentValues row = row(id);
            if (row != null && !"1".equals(row.getAsString("is_pending"))) {
                final ContentValues change = new ContentValues();
                change.put("_size", fileOf(row.getAsString("_data")).length());
                database().update("files", change, "_id = ?", new String[] {Long.toString(id)});
            }
        }
    }

    private ContentValues row(long id) {
        try (Cursor cursor = database().query("files", null, "_id = ?", new String[] {Long.toString(id)}, null, null,
                null)) {
            if (!cursor.moveToFirst()) {
                return null;
            }
            final ContentValues row = new ContentValues();
            for (final String column : cursor.getColumnNames()) {
                row.put(column, cursor.getString(cursor.getColumnIndexOrThrow(column)));
            }
            return row;
        }
    }

    /** A stream the app wrote through closed: the row's size is the file's, and a finished item goes to Photos. */
    private void finished(long id, String collection) {
        mWriting.remove(id);
        final ContentValues row = row(id);
        if (row == null) {
            return;
        }
        final File file = fileOf(row.getAsString("_data"));
        final ContentValues change = new ContentValues();
        change.put("_size", file.length());
        change.put("date_modified", System.currentTimeMillis() / 1000);
        database().update("files", change, "_id = ?", new String[] {Long.toString(id)});
        if (!"1".equals(row.getAsString("is_pending"))) {
            export(row, file);
        }
    }

    int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        final Target target = require(uri);
        final SQLiteDatabase database = database();
        final List<Long> ids = ids(target, selection, selectionArgs);
        int count = 0;
        for (final long id : ids) {
            final ContentValues row = row(id);
            final ContentValues change = new ContentValues();
            for (final String column : SETTABLE) {
                if (values.containsKey(column)) {
                    change.put(column, values.getAsString(column));
                }
            }
            if (values.containsKey("mime_type")) {
                change.put("mime_type", values.getAsString("mime_type"));
            }
            final boolean wasPending = "1".equals(row.getAsString("is_pending"));
            final Integer pending = values.getAsInteger("is_pending");
            final String relative = row.getAsString("relative_path");
            String name = row.getAsString("_display_name");
            if (values.containsKey("_display_name")) {
                name = MediaRules.fileName(FileUtils.buildValidFatFilename(values.getAsString("_display_name")),
                        change.containsKey("mime_type") ? change.getAsString("mime_type")
                        : row.getAsString("mime_type"));
                change.put("_display_name", name);
            }
            File file = fileOf(row.getAsString("_data"));
            if (wasPending && pending != null && pending == 0) {
                final File finalFile = new File(file.getParentFile(), name);
                if (file.isFile() && !file.renameTo(finalFile)) {
                    throw new IllegalStateException("Cannot move " + file + " to " + finalFile);
                }
                file = finalFile;
                change.put("_data", finalFile.getPath());
                change.put("is_pending", 0);
                change.putNull("date_expires");
                if (!change.containsKey("title")) {
                    change.put("title", MediaRules.title(name));
                }
                change.put("_size", file.length());
            }
            if (relative != null && !change.containsKey("_data") && values.containsKey("_display_name")) {
                final File renamed = new File(file.getParentFile(), name);
                if (file.isFile() && !file.renameTo(renamed)) {
                    throw new IllegalStateException("Cannot move " + file + " to " + renamed);
                }
                change.put("_data", renamed.getPath());
            }
            if (change.size() > 0) {
                count += database.update("files", change, "_id = ?", new String[] {Long.toString(id)});
            }
            if (wasPending && pending != null && pending == 0 && file.isFile()) {
                export(row(id), file);
            }
        }
        return count;
    }

    private List<Long> ids(Target target, String selection, String[] selectionArgs) {
        final List<Long> ids = new java.util.ArrayList<>();
        try (Cursor cursor = database().query("files", new String[] {"_id"}, where(target, selection), selectionArgs,
                null, null, null)) {
            while (cursor.moveToNext()) {
                ids.add(cursor.getLong(0));
            }
        }
        return ids;
    }

    int delete(Uri uri, String selection, String[] selectionArgs) {
        final Target target = require(uri);
        final List<Long> ids = ids(target, selection, selectionArgs);
        if (target.id >= 0 && ids.isEmpty()) {
            throw new SecurityException(mContext.getPackageName() + " has no access to " + uri);
        }
        for (final long id : ids) {
            final ContentValues row = row(id);
            fileOf(row.getAsString("_data")).delete();
            database().delete("files", "_id = ?", new String[] {Long.toString(id)});
        }
        return ids.size();
    }

    /** Asks the host to add a finished image or video to Photos (decision 5); other kinds stay in the app's folder. */
    private void export(ContentValues row, File file) {
        final String type = row.getAsString("media_type");
        if (!file.isFile() || !("1".equals(type) || "3".equals(type))) {
            return;
        }
        final String prefix = UriGrants.folder().getParentFile().getPath() + "/";
        if (!file.getPath().startsWith(prefix)) {
            return;
        }
        final int error = ActivityThread.requestExportMedia("1".equals(type) ? 1 : 2,
                file.getPath().substring(prefix.length()));
        if (error != 0 && error != -OsConstants.EOPNOTSUPP) {
            Log.w(TAG, "The host refused the export of " + file.getName() + ": " + error);
        }
    }
}
