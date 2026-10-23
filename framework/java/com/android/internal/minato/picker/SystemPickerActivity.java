package com.android.internal.minato.picker;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;

import com.android.minato.picker.PhotoManifest;
import com.android.minato.picker.PickLocation;
import com.android.minato.picker.PickManifest;
import com.android.minato.picker.PickerRequest;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The system picker (ADR-0057 decision 1): the activity that stands for Android's DocumentsUI and photo picker when an
 * app starts one of the five picker intents for a result. It asks the host for the pick over the session and waits;
 * the host answers with PICK_RESULT, and the activity returns that result to the app's activity and finishes. A
 * cancel is RESULT_CANCELED without data. Without a session there is no host to ask, and the result is a cancel at
 * once.
 * A pick of documents (step b) comes with the manifest the host wrote: the names of the clones in Download, which are
 * documents of the externalstorage provider (ExternalStorageProvider) with the id "primary:Download/<name>". The
 * result is as DocumentsUI's [measured: documents-test on apk-runner-35]: one pick in the data, several in a ClipData
 * without data, RESULT_OK; ACTION_OPEN_DOCUMENT's flags are read, write and persistable, ACTION_GET_CONTENT's read
 * only. The app holds read, write and may keep the grant in both cases (Android's DocumentsUI does not pass its own
 * grant on in the flags), so the grants are made with all three.
 */
public final class SystemPickerActivity extends Activity {
    private static final String TAG = "SystemPicker";
    private static final String EXTERNAL_STORAGE_AUTHORITY = DocumentsContract.EXTERNAL_STORAGE_PROVIDER_AUTHORITY;
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, SystemPickerActivity> sWaiting = new HashMap<>();

    private long mId;
    private int mKind;
    /** The photo picker's maximum number of picks. */
    private int mMaximum;
    private String[] mTypes;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Intent intent = getIntent();
        final int kind = PickerIntents.kindOf(intent);
        mKind = kind;
        mMaximum = kind == PickerIntents.PICK_IMAGES
                ? Math.max(1, Math.min(intent.getIntExtra(PickerIntents.EXTRA_PICK_IMAGES_MAX, 1), 1000)) : 0;
        final String[] extraTypes = intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES);
        mTypes = extraTypes != null && extraTypes.length > 0 ? extraTypes
                : new String[] {intent.getType() != null ? intent.getType() : "*/*"};
        mId = sNextId.getAndIncrement();
        sWaiting.put(mId, this);
        final int error = kind == PickerIntents.NONE ? -EOPNOTSUPP
                : ActivityThread.requestPick(mId, requestText(intent, kind));
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the pick request: " + error);
            }
            sWaiting.remove(mId);
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    /** The request's text from the app's intent, inside the limits the runtime enforces. */
    private static String requestText(Intent intent, int kind) {
        final int max = kind == PickerIntents.PICK_IMAGES
                ? intent.getIntExtra(PickerIntents.EXTRA_PICK_IMAGES_MAX, 1) : 0;
        return PickerRequest.text(kind, intent.getType(), intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES),
                intent.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false), max,
                intent.getStringExtra(Intent.EXTRA_TITLE));
    }

    /** The host's answer to request `id`: PICK_RESULT's result code and number of picks. */
    public static void onResult(long id, int resultCode, int count) {
        final SystemPickerActivity picker = sWaiting.remove(id);
        if (picker == null) {
            Log.w(TAG, "An answer for pick request " + id + " that nobody waits for");
            return;
        }
        picker.deliver(resultCode, count);
    }

    private void deliver(int resultCode, int count) {
        if (resultCode == RESULT_CANCELED || count == 0) {
            setResult(RESULT_CANCELED);
        } else {
            final Intent result = mKind == PickerIntents.OPEN_DOCUMENT || mKind == PickerIntents.GET_CONTENT
                    ? documents(count) : mKind == PickerIntents.PICK_IMAGES ? photos(count)
                    : mKind == PickerIntents.CREATE_DOCUMENT || mKind == PickerIntents.OPEN_DOCUMENT_TREE
                    ? location(count) : null;
            if (result == null) {
                Log.w(TAG, "The host picked " + count + " for a kind of picker without picks yet: a cancel");
                setResult(RESULT_CANCELED);
            } else {
                setResult(RESULT_OK, result);
            }
        }
        finish();
    }

    /**
     * The result for the host's `count` clones: read from the manifest of this request, which is deleted at once.
     * Null (a cancel) when there is no manifest, it is not exactly `count` plain names, or a name is no file in
     * Download.
     */
    private Intent documents(int count) {
        final File folder = UriGrants.folder();
        if (folder == null) {
            return null;
        }
        final File manifest = new File(new File(folder, "picks"), Long.toString(mId));
        final List<String> names;
        try {
            names = PickManifest.parse(readBounded(manifest), count);
        } catch (IOException e) {
            Log.w(TAG, "No manifest for pick " + mId + ": " + e);
            return null;
        } finally {
            manifest.delete();
        }
        if (names == null) {
            Log.w(TAG, "The manifest of pick " + mId + " is not " + count + " plain names");
            return null;
        }
        final List<Uri> uris = new ArrayList<>(names.size());
        for (final String name : names) {
            final File clone = new File(ExternalStorage.DOWNLOAD, name);
            if (!clone.isFile()) {
                Log.w(TAG, "The clone " + name + " of pick " + mId + " is no file in Download");
                UriGrants.cloned(names);
                return null;
            }
            uris.add(DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, "primary:Download/" + name));
        }
        UriGrants.cloned(names);
        final int held = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
        for (final Uri uri : uris) {
            UriGrants.grant(uri, held);
        }
        final Intent result = new Intent();
        result.addFlags(mKind == PickerIntents.OPEN_DOCUMENT ? held : Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if (uris.size() == 1) {
            result.setData(uris.get(0));
        } else {
            final ClipData clip = new ClipData(new ClipDescription("", mTypes), new ClipData.Item(uris.get(0)));
            for (int i = 1; i < uris.size(); i++) {
                clip.addItem(new ClipData.Item(uris.get(i)));
            }
            result.setClipData(clip);
        }
        return result;
    }

    /**
     * The result for a save or a folder (step d): the host created the file under the title in the folder the user
     * chose, or made the chosen folder visible, and wrote its path below /storage/emulated/0 as the manifest
     * (PickLocation). The result is the document's URI, or the tree's, with the user's grant.
     */
    private Intent location(int count) {
        final File folder = UriGrants.folder();
        if (folder == null || count != 1) {
            return null;
        }
        final File manifest = new File(new File(folder, "picks"), Long.toString(mId));
        final String path;
        try {
            path = PickLocation.parse(readBounded(manifest));
        } catch (IOException e) {
            Log.w(TAG, "No manifest for pick " + mId + ": " + e);
            return null;
        } finally {
            manifest.delete();
        }
        if (path == null) {
            Log.w(TAG, "The manifest of pick " + mId + " is not one plain path");
            return null;
        }
        final File target = new File(ExternalStorage.ROOT, path);
        final boolean tree = mKind == PickerIntents.OPEN_DOCUMENT_TREE;
        try {
            final String root = new File(ExternalStorage.ROOT).getCanonicalPath();
            if (!target.getCanonicalPath().startsWith(root + "/")) {
                return null;
            }
        } catch (IOException e) {
            return null;
        }
        if (tree ? !target.isDirectory() : !target.isFile()) {
            Log.w(TAG, "The place " + path + " of pick " + mId + " is not " + (tree ? "a folder" : "a file"));
            return null;
        }
        final String documentId = "primary:" + path;
        final int held = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | (tree ? Intent.FLAG_GRANT_PREFIX_URI_PERMISSION : 0);
        final Uri uri = tree ? DocumentsContract.buildTreeDocumentUri(EXTERNAL_STORAGE_AUTHORITY, documentId)
                : DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, documentId);
        UriGrants.grant(uri, held);
        return new Intent().setData(uri).addFlags(held);
    }

    /**
     * The result for the host's `count` photos and videos (step c): the clones in .minato/photos, listed in the
     * manifest with their sizes and times, become picker URIs of the provider MediaProvider serves. As the photo
     * picker returns them [measured: photos-test on apk-runner-35]: the flags read and persistable, one pick in the
     * data and, as the only item, in a ClipData too, several in a ClipData without data.
     */
    private Intent photos(int count) {
        final File folder = UriGrants.folder();
        if (folder == null) {
            return null;
        }
        final File manifest = new File(new File(folder, "picks"), Long.toString(mId));
        final List<PhotoManifest.Item> items;
        try {
            items = PhotoManifest.parse(readBounded(manifest), count);
        } catch (IOException e) {
            Log.w(TAG, "No manifest for pick " + mId + ": " + e);
            return null;
        } finally {
            manifest.delete();
        }
        if (items == null || items.size() > mMaximum) {
            Log.w(TAG, "The manifest of pick " + mId + " is not " + count + " photos within the maximum");
            return null;
        }
        final File photos = UriGrants.photoFolder();
        for (final PhotoManifest.Item item : items) {
            final File clone = new File(photos, item.name);
            if (!clone.isFile() || Files.isSymbolicLink(clone.toPath())) {
                Log.w(TAG, "The clone " + item.name + " of pick " + mId + " is no file in the photo folder");
                return null;
            }
        }
        final List<Uri> uris = new ArrayList<>(items.size());
        final List<String> types = new ArrayList<>();
        final int held = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
        for (final PhotoManifest.Item item : items) {
            final long id = UriGrants.photos().add(item);
            final Uri uri = Uri.parse(UriGrants.PHOTO_URI_PREFIX + id);
            UriGrants.grant(uri, held);
            uris.add(uri);
            if (!types.contains(item.mimeType())) {
                types.add(item.mimeType());
            }
        }
        UriGrants.savePhotos();
        final Intent result = new Intent();
        result.addFlags(held);
        final ClipData clip = new ClipData(new ClipDescription("", types.toArray(new String[0])),
                new ClipData.Item(uris.get(0)));
        for (int i = 1; i < uris.size(); i++) {
            clip.addItem(new ClipData.Item(uris.get(i)));
        }
        result.setClipData(clip);
        if (uris.size() == 1) {
            result.setData(uris.get(0));
        }
        return result;
    }

    /** The manifest's bytes; IOException for a file over the most a manifest has, or none. */
    private static byte[] readBounded(File file) throws IOException {
        if (!file.isFile() || Files.isSymbolicLink(file.toPath())) {
            throw new IOException("not a file");
        }
        try (InputStream in = new FileInputStream(file)) {
            final byte[] bytes = in.readNBytes(PhotoManifest.MAX_BYTES + 1);
            return bytes;
        }
    }

    @Override
    protected void onDestroy() {
        // Gone before the answer, for instance finishActivity(requestCode): the host drops the picker.
        if (sWaiting.remove(mId) != null) {
            ActivityThread.cancelPick(mId);
        }
        super.onDestroy();
    }
}
