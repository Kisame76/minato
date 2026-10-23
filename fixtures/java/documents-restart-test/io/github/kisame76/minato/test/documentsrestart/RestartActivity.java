package io.github.kisame76.minato.test.documentsrestart;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * ADR-0057 step b's restart test (Minato only: Android deletes no picked file). The first start opens one document
 * and keeps its grant, then opens two documents, one of them the same, and keeps nothing more. The second start,
 * over the same folders, sees what outlives the process: the kept grant reads, the other document's grant is gone,
 * and so is the clone nobody keeps in Download. Prints "documents-restart-test: ..." lines.
 */
public class RestartActivity extends Activity {
    private static final String TAG = "documents-restart-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private boolean mSecond;
    private int mNext;
    private Uri mKept;
    private Uri mOther;

    private static void out(String line) {
        System.out.println(TAG + ": " + line);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        File marker = new File(getFilesDir(), "started");
        mSecond = marker.exists();
        try {
            marker.createNewFile();
        } catch (java.io.IOException e) {
            out("marker: " + e);
        }
        mHandler.post(mSecond ? this::second : this::startNext);
    }

    private void startNext() {
        if (mNext == 2) {
            out("download: " + names());
            out("done");
            finish();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        if (mNext == 1) {
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        startActivityForResult(intent, mNext++);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        List<Uri> uris = new ArrayList<>();
        if (data != null && data.getData() != null) {
            uris.add(data.getData());
        }
        ClipData clip = data == null ? null : data.getClipData();
        for (int i = 0; clip != null && i < clip.getItemCount(); i++) {
            uris.add(clip.getItemAt(i).getUri());
        }
        out("first start, pick " + requestCode + ": " + uris.size());
        if (requestCode == 0) {
            mKept = uris.get(0);
            getContentResolver().takePersistableUriPermission(mKept, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            out("kept: " + name(mKept));
        } else {
            for (Uri uri : uris) {
                if (!uri.equals(mKept)) {
                    mOther = uri;
                    out("not kept: " + name(uri));
                }
            }
        }
        mHandler.post(this::startNext);
    }

    private String name(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {"_display_name"}, null, null, null)) {
            return cursor.moveToFirst() ? cursor.getString(0) : "none";
        }
    }

    private String names() {
        String[] names = new File("/storage/emulated/0/Download").list();
        if (names == null) {
            return "none";
        }
        Arrays.sort(names);
        return Arrays.toString(names);
    }

    /** The second start: what the first one left. */
    private void second() {
        ContentResolver resolver = getContentResolver();
        out("second start");
        List<UriPermission> kept = resolver.getPersistedUriPermissions();
        out("persisted: " + kept.size());
        for (UriPermission permission : kept) {
            out("persisted: " + name(permission.getUri()) + " read=" + permission.isReadPermission() + " write="
                    + permission.isWritePermission());
            try (InputStream in = resolver.openInputStream(permission.getUri())) {
                out("read: " + new String(in.readAllBytes()).replace("\n", "\\n"));
            } catch (Exception e) {
                out("read: " + e.getClass().getName());
            }
        }
        out("download: " + names());
        out("done");
        finish();
    }
}
