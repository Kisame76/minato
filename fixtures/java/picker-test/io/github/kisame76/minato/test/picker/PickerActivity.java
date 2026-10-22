package io.github.kisame76.minato.test.picker;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.Bundle;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import java.util.List;

/**
 * ADR-0057 step a's test: an app that handles none of the file picker intents resolves each, starts it for a result
 * and logs the lifecycle around the picker. The picker is cancelled each time (the Android reference presses BACK once
 * "start N" shows, the Mac's harness answers the picker request with a cancel), so each result is RESULT_CANCELED.
 * It runs from the first window focus and logs under the tag picker-test; tools/android-reference.sh takes the log
 * from Android 15, and tools/picker-check.sh compares Minato's with it.
 */
public class PickerActivity extends Activity {
    static final String TAG = "picker-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private boolean mResultPending;
    private int mNext;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private Intent intent(int index) {
        switch (index) {
            case 0:
                return new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                        .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/pdf", "image/*"})
                        .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            case 1:
                return new Intent(Intent.ACTION_GET_CONTENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("text/plain");
            case 2:
                // An emoji in the title: the request carries it as UTF-8, not as JNI's two surrogates.
                return new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("text/plain").putExtra(Intent.EXTRA_TITLE, "note \ud83d\ude00 \u00e4.txt");
            case 3:
                return new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            default:
                // MediaStore.ACTION_PICK_IMAGES and EXTRA_PICK_IMAGES_MAX, which javac inlines in an app
                return new Intent("android.provider.action.PICK_IMAGES")
                        .putExtra("android.provider.extra.PICK_IMAGES_MAX", 5);
        }
    }

    private static final String[] NAMES = {"OPEN_DOCUMENT", "GET_CONTENT", "CREATE_DOCUMENT", "OPEN_DOCUMENT_TREE",
        "PICK_IMAGES"};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        log("onCreate");
    }

    @Override
    protected void onStart() {
        super.onStart();
        log("onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        log("onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        log("onResume");
        if (mResultPending) {
            mResultPending = false;
            mHandler.post(this::startNext);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        log("onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        log("onStop");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("onActivityResult " + NAMES[requestCode] + " result=" + resultCode + " data="
                + (data == null ? "null" : "set"));
        mResultPending = true;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            resolve();
            clip();
            startNext();
        });
    }

    /** What resolves each intent, the way an app asks first. */
    private void resolve() {
        PackageManager pm = getPackageManager();
        for (int i = 0; i < NAMES.length; i++) {
            Intent intent = intent(i);
            ResolveInfo info = pm.resolveActivity(intent, 0);
            List<ResolveInfo> all = pm.queryIntentActivities(intent, 0);
            log("resolve " + NAMES[i] + ": " + (info == null ? "null" : info.activityInfo.packageName + "/"
                    + info.activityInfo.name) + " all=" + all.size());
        }
    }

    /** The ClipData a picker answers several picks with: an item per URI, the description's MIME types, a parcel. */
    private void clip() {
        try {
            Uri first = Uri.parse("content://com.example.documents/document/1");
            ClipData clip = ClipData.newRawUri("picks", first);
            clip.addItem(new ClipData.Item(Uri.parse("content://com.example.documents/document/2")));
            Intent result = new Intent().setData(first)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            result.setClipData(clip);
            Parcel parcel = Parcel.obtain();
            result.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            Intent back = Intent.CREATOR.createFromParcel(parcel);
            parcel.recycle();
            ClipData got = back.getClipData();
            log("clip: items=" + got.getItemCount() + " first=" + got.getItemAt(0).getUri() + " second="
                    + got.getItemAt(1).getUri() + " label=" + got.getDescription().getLabel() + " flags="
                    + back.getFlags() + " data=" + back.getData());
        } catch (RuntimeException e) {
            log("clip: " + e.getClass().getName());
        }
    }

    private void startNext() {
        if (mNext >= NAMES.length) {
            log("done");
            finish();
            return;
        }
        int index = mNext++;
        log("start " + NAMES[index]);
        try {
            startActivityForResult(intent(index), index);
        } catch (RuntimeException e) {
            log("start " + NAMES[index] + ": " + e.getClass().getName());
            mHandler.post(this::startNext);
        }
    }
}
