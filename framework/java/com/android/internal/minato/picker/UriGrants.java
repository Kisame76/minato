package com.android.internal.minato.picker;

import android.app.ActivityThread;
import android.app.Application;
import android.content.UriPermission;
import android.net.Uri;
import android.util.Log;

import com.android.minato.picker.GrantTable;
import com.android.minato.picker.PhotoIndex;
import com.android.minato.picker.PickClones;
import com.android.minato.picker.PickManifest;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The URI grants of the app (ADR-0057 step b), which Android's system_server keeps for it: the system picker grants the
 * documents it returns, ContextImpl's checkUriPermission and the documents provider ask, LocalUriGrants answers
 * takePersistableUriPermission and the others. Only a kept grant outlives the process: they are stored in the app's
 * data folder, <data>/.minato/uri-grants, and at the first use of a start the clones of picks that no kept grant names
 * are deleted from Download (decision 2). The table itself is com.android.minato.picker.GrantTable.
 */
public final class UriGrants {
    private static final String TAG = "UriGrants";
    /** The authority of the photo picker's provider, as it is in the picker URIs. */
    public static final String PHOTO_AUTHORITY = "com.android.providers.media.photopicker";
    /** The folder the host and the framework share in the app's data folder (guest_interface.h). */
    static final String FOLDER = ".minato";
    private static final String DOCUMENTS_PREFIX = "/document/primary:Download/";
    /** The URIs the picker grants: buildDocumentUri's form of "primary:Download/<name>". */
    private static final String DOCUMENT_URI_PREFIX =
            "content://com.android.externalstorage.documents/document/primary%3A";
    /** The tree URIs the picker grants for a folder the user chose. */
    private static final String TREE_URI_PREFIX = "content://com.android.externalstorage.documents/tree/primary%3A";
    /** The URIs the photo picker grants: content://media/picker/0/<its authority>/media/<ID>. */
    public static final String PHOTO_URI_PREFIX = "content://media/picker/0/" + PHOTO_AUTHORITY + "/media/";
    /** The most bytes of the kept grants that are read: 1000 picks a time are far below. */
    private static final int MAX_FILE_BYTES = 1 << 20;

    private static GrantTable sTable;
    private static File sFolder;

    private UriGrants() {
    }

    /** The app's .minato folder, null while there is no application. */
    static synchronized File folder() {
        if (sFolder == null) {
            final Application application = ActivityThread.currentApplication();
            if (application == null) {
                return null;
            }
            sFolder = new File(application.getDataDir(), FOLDER);
        }
        return sFolder;
    }

    private static synchronized GrantTable table() {
        if (sTable == null) {
            sTable = new GrantTable();
            final File folder = folder();
            if (folder != null) {
                try {
                    final File file = new File(folder, "uri-grants");
                    // The app can write the file, and only the documents of the picker are its to keep.
                    if (!Files.isSymbolicLink(file.toPath()) && file.isFile() && file.length() <= MAX_FILE_BYTES) {
                        sTable = GrantTable.parse(new String(Files.readAllBytes(file.toPath()),
                                StandardCharsets.UTF_8), DOCUMENT_URI_PREFIX, TREE_URI_PREFIX,
                                PHOTO_URI_PREFIX);
                    }
                } catch (IOException | RuntimeException e) {
                    Log.w(TAG, "Cannot read the kept grants: " + e);
                }
                try {
                    dropUnkeptClones(folder);
                } catch (IOException | RuntimeException e) {
                    Log.w(TAG, "Cannot clean the clones: " + e);
                }
                dropUnkeptPhotos(folder);
            }
        }
        return sTable;
    }

    /** The clones of earlier picks that no kept grant names go, as decision 2 says. */
    private static void dropUnkeptClones(File folder) throws IOException {
        final Set<String> keep = new HashSet<>();
        for (final GrantTable.Entry entry : sTable.persisted()) {
            final String path = Uri.parse(entry.uri).getPath();
            if (path != null && path.startsWith(DOCUMENTS_PREFIX)) {
                keep.add(path.substring(DOCUMENTS_PREFIX.length()));
            }
        }
        PickClones.dropUnkept(new File(ExternalStorage.DOWNLOAD), new File(folder, "clones"), keep);
    }

    /** The folder of the photo picker's clones, below the app's .minato folder. */
    public static File photoFolder() {
        final File folder = folder();
        return folder == null ? null : new File(folder, "photos");
    }

    private static PhotoIndex sPhotos;

    /** The photos the picker returned, as stored; the clones that no kept grant names went at this start. */
    public static synchronized PhotoIndex photos() {
        table();
        if (sPhotos == null) {
            sPhotos = new PhotoIndex();
        }
        return sPhotos;
    }

    private static void dropUnkeptPhotos(File folder) {
        final File file = new File(folder, "photos.index");
        sPhotos = new PhotoIndex();
        try {
            if (!Files.isSymbolicLink(file.toPath()) && file.isFile() && file.length() <= MAX_FILE_BYTES) {
                sPhotos = PhotoIndex.parse(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Cannot read the photo index: " + e);
        }
        final Set<Long> keep = new HashSet<>();
        for (final GrantTable.Entry entry : sTable.persisted()) {
            if (entry.uri.startsWith(PHOTO_URI_PREFIX)
                    && entry.uri.substring(PHOTO_URI_PREFIX.length()).matches("[0-9]{10,18}")) {
                keep.add(Long.parseLong(entry.uri.substring(PHOTO_URI_PREFIX.length())));
            }
        }
        final File photos = new File(folder, "photos");
        for (final String name : sPhotos.dropUnkept(keep)) {
            final File clone = new File(photos, name);
            if (PickManifest.plainName(name) && !Files.isSymbolicLink(clone.toPath()) && clone.isFile()) {
                clone.delete();
            }
        }
        savePhotos();
    }

    /** Stores the photo index. */
    public static synchronized void savePhotos() {
        final File folder = folder();
        if (folder == null || sPhotos == null) {
            return;
        }
        try {
            folder.mkdirs();
            final File partial = new File(folder, "photos.index.partial");
            Files.write(partial.toPath(), sPhotos.serialize().getBytes(StandardCharsets.UTF_8));
            Files.move(partial.toPath(), new File(folder, "photos.index").toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException e) {
            Log.w(TAG, "Cannot store the photo index: " + e);
        }
    }

    private static void save() {
        final File folder = folder();
        if (folder == null) {
            return;
        }
        try {
            folder.mkdirs();
            final File partial = new File(folder, "uri-grants.partial");
            Files.write(partial.toPath(), sTable.serialize().getBytes(StandardCharsets.UTF_8));
            Files.move(partial.toPath(), new File(folder, "uri-grants").toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException e) {
            Log.w(TAG, "Cannot store the kept grants: " + e);
        }
    }

    /** Once per start, at the Application's creation: reads the kept grants and removes the clones nobody keeps. */
    public static void startUp() {
        table();
    }

    /** Grants `uri` the modes of `flags` (Intent's FLAG_GRANT_*). */
    public static void grant(Uri uri, int flags) {
        table().grant(uri.toString(), flags);
    }

    /** Whether the app holds `modes` (Intent.FLAG_GRANT_READ_URI_PERMISSION and WRITE) for `uri`. */
    public static boolean allows(Uri uri, int modes) {
        return table().allows(uri.toString(), modes);
    }

    /** UriGrantsManager's takePersistableUriPermission: SecurityException without a grant that allows it. */
    public static void take(Uri uri, int modes) {
        table().take(uri.toString(), modes, System.currentTimeMillis());
        save();
    }

    public static void release(Uri uri, int modes) {
        table().release(uri.toString(), modes);
        save();
    }

    /** The kept grants, as ContentResolver.getPersistedUriPermissions gives them. */
    public static List<UriPermission> persisted() {
        final List<UriPermission> list = new ArrayList<>();
        for (final GrantTable.Entry entry : table().persisted()) {
            list.add(new UriPermission(Uri.parse(entry.uri), entry.modes, entry.time));
        }
        return list;
    }

    /** Remembers the names of clones the host made for a pick, for the next start's cleaning. */
    static void cloned(List<String> names) {
        // The earlier starts' clones go at the first use of the grants: before this list gets the new ones.
        table();
        final File folder = folder();
        if (folder == null) {
            return;
        }
        try {
            PickClones.add(new File(folder, "clones"), names);
        } catch (IOException e) {
            Log.w(TAG, "Cannot list the clones: " + e);
        }
    }
}
