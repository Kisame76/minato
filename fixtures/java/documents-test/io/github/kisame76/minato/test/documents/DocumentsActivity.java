package io.github.kisame76.minato.test.documents;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0057 step b's test: an app reads a file with the system picker. It first asks the documents provider for a
 * document it holds no grant for, then opens one document, several and one with ACTION_GET_CONTENT for a result and
 * logs everything it can see of each pick: the grant flags, the URI's form, the provider's row (columns in order, the time masked), the type, the
 * bytes' SHA-256 and the persisted grants. The picked files are known.txt and data.bin in Download (Android: pushed to
 * /sdcard/Download and picked by hand-free taps; the Mac's harness answers the request with the same files). Logs under
 * the tag documents-test; tools/android-reference.sh takes the log from Android 15 and tools/documents-check.sh
 * compares Minato's with it.
 */
public class DocumentsActivity extends Activity {
    static final String TAG = "documents-test";
    static final String KNOWN = "content://com.android.externalstorage.documents/document/primary%3ADownload%2Fknown.txt";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mNext;
    private static final String[] NAMES = {"single", "multiple", "get-content"};

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
        mHandler.post(() -> {
            ungranted();
            startNext();
        });
    }

    /** A document of the documents provider the app holds no grant for. */
    private void ungranted() {
        ContentResolver resolver = getContentResolver();
        Uri uri = Uri.parse(KNOWN);
        try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
            log("ungranted query: " + (cursor == null ? "null" : "cursor"));
        } catch (RuntimeException e) {
            log("ungranted query: " + type(e));
        }
        try (InputStream in = resolver.openInputStream(uri)) {
            log("ungranted open: " + (in == null ? "null" : "stream"));
        } catch (Exception e) {
            log("ungranted open: " + type(e));
        }
        try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            log("ungranted persist: ok");
        } catch (RuntimeException e) {
            log("ungranted persist: " + type(e));
        }
        log("ungranted persisted: " + resolver.getPersistedUriPermissions().size());
        log("ungranted check: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION));
    }

    private void startNext() {
        if (mNext == NAMES.length) {
            log("done");
            finish();
            return;
        }
        int index = mNext++;
        Intent intent = new Intent(index == 2 ? Intent.ACTION_GET_CONTENT : Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        if (index == 1) {
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        log("start " + NAMES[index]);
        startActivityForResult(intent, index);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("result " + NAMES[requestCode] + ": code=" + resultCode + " data="
                + (data == null ? "null" : "set"));
        if (data != null) {
            int grant = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
            log("flags: " + (data.getFlags() & grant));
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            log("data: " + (data.getData() == null ? "null" : "uri") + " clip: "
                    + (clip == null ? "null" : String.valueOf(clip.getItemCount())));
            if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) {
                    if (!uris.contains(clip.getItemAt(i).getUri())) {
                        uris.add(clip.getItemAt(i).getUri());
                    }
                }
            }
            for (Uri uri : uris) {
                dump(uri);
            }
        }
        mHandler.post(this::startNext);
    }

    /** What the app sees of one picked document. */
    private void dump(Uri uri) {
        ContentResolver resolver = getContentResolver();
        log("uri: " + uri.getScheme() + "://" + uri.getAuthority() + " " + uri.getPath());
        try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
            StringBuilder row = new StringBuilder("row:");
            if (cursor == null || !cursor.moveToFirst()) {
                row.append(" none");
            } else {
                for (int i = 0; i < cursor.getColumnCount(); i++) {
                    String name = cursor.getColumnName(i);
                    // The time of the file differs from run to run.
                    String value = name.equals("last_modified") ? (cursor.getLong(i) > 0 ? "<set>" : "0")
                            : cursor.getString(i);
                    row.append(i == 0 ? " " : ", ").append(name).append('=').append(value);
                }
            }
            log(row.toString());
        } catch (RuntimeException e) {
            log("row: " + type(e));
        }
        try (Cursor cursor = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE},
                null, null, null)) {
            cursor.moveToFirst();
            log("openable: " + cursor.getString(0) + " " + cursor.getLong(1));
        } catch (RuntimeException e) {
            log("openable: " + type(e));
        }
        log("type: " + resolver.getType(uri));
        try (InputStream in = resolver.openInputStream(uri)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[4096];
            int total = 0;
            for (int n; (n = in.read(buffer)) > 0; total += n) {
                digest.update(buffer, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest()) {
                hex.append(String.format("%02x", b));
            }
            log("read: " + total + " " + hex);
        } catch (Exception e) {
            log("read: " + type(e));
        }
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "r")) {
            log("fd: " + fd.getStatSize());
        } catch (Exception e) {
            log("fd: " + type(e));
        }
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "rw")) {
            log("write fd: opened");
        } catch (Exception e) {
            log("write fd: " + type(e));
        }
        log("check read: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                + " write: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
        try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            for (UriPermission p : resolver.getPersistedUriPermissions()) {
                log("persisted: " + p.getUri().getPath() + " read=" + p.isReadPermission() + " write="
                        + p.isWritePermission() + " time=" + (p.getPersistedTime() > 0 ? "<set>" : "0"));
            }
            resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            log("released: " + resolver.getPersistedUriPermissions().size());
        } catch (RuntimeException e) {
            log("persist: " + type(e));
        }
    }
}
