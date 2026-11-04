package io.github.kisame76.minato.test.documentstree;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * ADR-0057 step d's folder test: an app opens Documents with ACTION_OPEN_DOCUMENT_TREE and EXTRA_INITIAL_URI, as a
 * game that keeps its saves there does, keeps the grant, makes a file in the folder and writes it; at its next start
 * it finds the kept grant, reads the file through it, deletes it and lets the grant go. The user allows the folder
 * (Android: DocumentsUI's "Use this folder" and "Allow"; Minato: its alert's "Allow"). Logs under the tag
 * documents-tree-test; tools/android-reference.sh takes the log of both starts from Android 15, and
 * tools/java-start-check.sh compares Minato's with it.
 */
public class DocumentsTreeActivity extends Activity {
    static final String TAG = "documents-tree-test";
    static final String NAME = "documents-tree-test.txt";
    private static final int GRANT = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String type(Throwable e) {
        return e.getClass().getName();
    }

    private static String form(Uri uri) {
        return uri.getScheme() + "://" + uri.getAuthority() + " " + uri.getPath();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(this::begin);
    }

    private void begin() {
        List<UriPermission> kept = getContentResolver().getPersistedUriPermissions();
        log("kept: " + kept.size());
        if (kept.isEmpty()) {
            Uri documents = DocumentsContract.buildDocumentUri("com.android.externalstorage.documents",
                    "primary:Documents");
            log("start tree");
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                    .putExtra(DocumentsContract.EXTRA_INITIAL_URI, documents), 1);
            return;
        }
        UriPermission grant = kept.get(0);
        log("kept tree: " + form(grant.getUri()) + " read=" + grant.isReadPermission() + " write="
                + grant.isWritePermission());
        again(grant.getUri());
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("result: code=" + resultCode + " data=" + (data == null ? "null" : "set"));
        if (data == null || data.getData() == null) {
            done();
            return;
        }
        Uri tree = data.getData();
        int flags = GRANT | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
        log("flags: " + (data.getFlags() & flags));
        log("tree: " + form(tree));
        try {
            getContentResolver().takePersistableUriPermission(tree, GRANT);
            log("persist: ok " + getContentResolver().getPersistedUriPermissions().size());
        } catch (RuntimeException e) {
            log("persist: " + type(e));
        }
        first(tree);
    }

    /** The first start: the file made in the folder, written and read. */
    private void first(Uri tree) {
        ContentResolver resolver = getContentResolver();
        String treeId = DocumentsContract.getTreeDocumentId(tree);
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, treeId);
        try {
            Uri made = DocumentsContract.createDocument(resolver, root, "text/plain", NAME);
            log("created: " + (made == null ? "null" : form(made)));
            if (made != null) {
                try (OutputStream out = resolver.openOutputStream(made, "wt")) {
                    out.write("level 3\n".getBytes(StandardCharsets.UTF_8));
                    log("write: ok");
                }
                log("read: " + read(made));
            }
            log("listed: " + listed(tree, treeId));
        } catch (Exception e) {
            log("create: " + type(e));
        }
        missing(tree, treeId);
        done();
    }

    /** A document that is not there, below the granted folder: what reading, querying and typing it throw or give. */
    private void missing(Uri tree, String treeId) {
        Uri gone = DocumentsContract.buildDocumentUriUsingTree(tree, treeId + "/documents-tree-test-missing.txt");
        log("missing read: " + read(gone));
        try (Cursor cursor = getContentResolver().query(gone,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            log("missing query: " + (cursor == null ? "null" : "rows " + cursor.getCount()));
        } catch (Exception e) {
            log("missing query: " + type(e));
        }
        try {
            log("missing type: " + getContentResolver().getType(gone));
        } catch (Exception e) {
            log("missing type: " + type(e));
        }
    }

    /** The next start: the file read through the kept grant, then deleted, and the grant let go. */
    private void again(Uri tree) {
        ContentResolver resolver = getContentResolver();
        String treeId = DocumentsContract.getTreeDocumentId(tree);
        Uri file = DocumentsContract.buildDocumentUriUsingTree(tree, treeId + "/" + NAME);
        log("read again: " + read(file));
        try {
            log("deleted: " + DocumentsContract.deleteDocument(resolver, file));
            log("listed after delete: " + listed(tree, treeId));
        } catch (Exception e) {
            log("delete: " + type(e));
        }
        resolver.releasePersistableUriPermission(tree, GRANT);
        log("released: " + resolver.getPersistedUriPermissions().size());
        done();
    }

    private void done() {
        log("done");
        finish();
    }

    private String read(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8).replace("\n", "\\n");
        } catch (Exception e) {
            return type(e);
        }
    }

    /** How many of the folder's children have the test's name: the folder may hold other files. */
    private int listed(Uri tree, String treeId) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, treeId);
        int count = 0;
        try (Cursor cursor = getContentResolver().query(children,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                if (NAME.equals(cursor.getString(0))) {
                    count++;
                }
            }
        }
        return count;
    }
}
