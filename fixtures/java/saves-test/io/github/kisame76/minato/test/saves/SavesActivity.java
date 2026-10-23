package io.github.kisame76.minato.test.saves;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * ADR-0057 step d's test: an app saves a document (ACTION_CREATE_DOCUMENT) and works in a folder the user opened for it
 * (ACTION_OPEN_DOCUMENT_TREE), and logs what it sees: the result, its grant flags, the URIs' forms, the provider's rows,
 * the app's writes and reads, and the calls of DocumentsContract (create, rename, delete) with their answers. The user
 * saves "note.txt" into Download (Android: by taps on DocumentsUI; the Mac's harness answers the request with a folder)
 * and opens the folder "treetest" with a.txt and b.txt in it. Logs under the tag saves-test; tools/android-reference.sh
 * takes the log from Android 15 and tools/saves-check.sh compares Minato's with it.
 */
public class SavesActivity extends Activity {
    static final String TAG = "saves-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mNext;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String type(Throwable e) {
        return e.getClass().getName();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(this::startNext);
    }

    private void startNext() {
        if (mNext == 2) {
            log("done");
            finish();
            return;
        }
        int index = mNext++;
        Intent intent = index == 0
                ? new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("text/plain").putExtra(Intent.EXTRA_TITLE, "note.txt")
                : new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        log("start " + (index == 0 ? "create" : "tree"));
        startActivityForResult(intent, index);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("result " + (requestCode == 0 ? "create" : "tree") + ": code=" + resultCode + " data="
                + (data == null ? "null" : "set"));
        if (data != null && data.getData() != null) {
            int grant = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
            log("flags: " + (data.getFlags() & grant) + " clip: " + (data.getClipData() == null ? "null" : "set"));
            if (requestCode == 0) {
                created(data.getData());
            } else {
                tree(data.getData());
            }
        }
        mHandler.post(this::startNext);
    }

    private static String form(Uri uri) {
        return uri.getScheme() + "://" + uri.getAuthority() + " " + uri.getPath();
    }

    /** A cursor's first row in the content tool's form, the time masked. */
    private String row(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor == null) {
                return "null";
            }
            if (!cursor.moveToFirst()) {
                return "none";
            }
            StringBuilder row = new StringBuilder();
            for (int i = 0; i < cursor.getColumnCount(); i++) {
                String name = cursor.getColumnName(i);
                String value = name.equals("last_modified") ? (cursor.getLong(i) > 0 ? "<set>" : "0")
                        : cursor.getString(i);
                row.append(i == 0 ? "" : ", ").append(name).append('=').append(value);
            }
            return row.toString();
        } catch (RuntimeException e) {
            return type(e);
        }
    }

    private String read(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8).replace("\n", "\\n");
        } catch (Exception e) {
            return type(e);
        }
    }

    private void write(Uri uri, String text) {
        try (OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            log("write: ok");
        } catch (Exception e) {
            log("write: " + type(e));
        }
    }

    /** ACTION_CREATE_DOCUMENT's result: the new document, written, renamed and deleted by the app. */
    private void created(Uri uri) {
        log("uri: " + form(uri));
        log("row: " + row(uri));
        log("document: " + DocumentsContract.isDocumentUri(this, uri) + " " + DocumentsContract.getDocumentId(uri));
        write(uri, "saved text\n");
        log("read: " + read(uri));
        log("row after: " + row(uri));
        try {
            Uri renamed = DocumentsContract.renameDocument(getContentResolver(), uri, "renamed.txt");
            log("renamed: " + (renamed == null ? "null" : form(renamed)));
            if (renamed != null) {
                log("renamed read: " + read(renamed));
                log("deleted: " + DocumentsContract.deleteDocument(getContentResolver(), renamed));
                log("row deleted: " + row(renamed));
            }
        } catch (Exception e) {
            log("rename: " + type(e));
        }
    }

    /** ACTION_OPEN_DOCUMENT_TREE's result: the folder listed, a document made in it, renamed and deleted. */
    private void tree(Uri tree) {
        ContentResolver resolver = getContentResolver();
        log("tree: " + form(tree));
        String treeId = DocumentsContract.getTreeDocumentId(tree);
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, treeId);
        log("root: " + form(root));
        log("root row: " + row(root));
        listing(tree, treeId, "children");
        try {
            Uri made = DocumentsContract.createDocument(resolver, root, "text/plain", "c.txt");
            log("created: " + (made == null ? "null" : form(made)));
            if (made != null) {
                write(made, "gamma\n");
                log("created read: " + read(made));
                listing(tree, treeId, "children after create");
                Uri renamed = DocumentsContract.renameDocument(resolver, made, "d.txt");
                log("renamed: " + (renamed == null ? "null" : form(renamed)));
                if (renamed != null) {
                    log("deleted: " + DocumentsContract.deleteDocument(resolver, renamed));
                }
                listing(tree, treeId, "children after delete");
            }
        } catch (Exception e) {
            log("create: " + type(e));
        }
        // Names an app should not be able to turn into other places: the provider makes them plain file names.
        try {
            Uri odd = DocumentsContract.createDocument(resolver, root, "text/plain", "../odd/x.txt");
            log("odd created: " + (odd == null ? "null" : form(odd)));
            if (odd != null) {
                Uri oddRenamed = DocumentsContract.renameDocument(resolver, odd, "..");
                log("odd renamed: " + (oddRenamed == null ? "null" : form(oddRenamed)));
                oddRenamed = oddRenamed == null ? odd : oddRenamed;
                Uri slash = DocumentsContract.renameDocument(resolver, oddRenamed, "a/b:c?.txt");
                log("odd renamed again: " + (slash == null ? "null" : form(slash)));
                log("odd deleted: " + DocumentsContract.deleteDocument(resolver, slash == null ? oddRenamed : slash));
            }
            listing(tree, treeId, "children after odd");
        } catch (Exception e) {
            log("odd: " + type(e));
        }
        try {
            Uri a = DocumentsContract.buildDocumentUriUsingTree(tree, treeId + "/a.txt");
            log("a read: " + read(a));
            log("child of root: " + DocumentsContract.isChildDocument(resolver, root, a));
            log("a in tree: " + DocumentsContract.isDocumentUri(this, a));
        } catch (Exception e) {
            log("a: " + type(e));
        }
        try {
            resolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            log("persist: ok " + resolver.getPersistedUriPermissions().size());
            resolver.releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            log("released: " + resolver.getPersistedUriPermissions().size());
        } catch (RuntimeException e) {
            log("persist: " + type(e));
        }
    }

    /** The folder's children by name, sorted, with their rows. */
    private void listing(Uri tree, String treeId, String label) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, treeId);
        List<String> rows = new ArrayList<>();
        try (Cursor cursor = getContentResolver().query(children, null, null, null, null)) {
            if (cursor == null) {
                log(label + ": null");
                return;
            }
            while (cursor.moveToNext()) {
                StringBuilder row = new StringBuilder();
                for (int i = 0; i < cursor.getColumnCount(); i++) {
                    String name = cursor.getColumnName(i);
                    row.append(i == 0 ? "" : ", ").append(name).append('=')
                            .append(name.equals("last_modified") ? "<set>" : cursor.getString(i));
                }
                rows.add(row.toString());
            }
        } catch (RuntimeException e) {
            log(label + ": " + type(e));
            return;
        }
        Collections.sort(rows);
        log(label + ": " + rows.size());
        for (String row : rows) {
            log("  " + row);
        }
    }
}
