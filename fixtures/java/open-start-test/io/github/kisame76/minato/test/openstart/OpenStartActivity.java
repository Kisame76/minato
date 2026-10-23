package io.github.kisame76.minato.test.openstart;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;

/**
 * ADR-0062's test, started: an app that handles none of the intents that leave it for iOS starts each for a result,
 * one after the other, and logs the result. The host (apk-run's model answer on the Mac) answers each request with
 * RESULT_OK; what it got is checked in order by tools/open-check.sh from apk-run's lines. Runtime-only: on Android each
 * would open an app of the emulator.
 */
public class OpenStartActivity extends Activity {
    static final String TAG = "open-start-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mNext;

    private static final String[] NAMES = {"view https", "view tel", "view geo", "sendto mailto", "sendto smsto",
        "chooser of send text", "send a file", "view market", "send multiple"};

    private Intent intent(int index) {
        final Uri report = Uri.parse("content://io.github.kisame76.minato.test.openstart.files/report.txt");
        final Uri image = Uri.parse("content://io.github.kisame76.minato.test.openstart.files/a%20b.png");
        switch (index) {
            case 0:
                return new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/a b?q=ü"));
            case 1:
                return new Intent(Intent.ACTION_VIEW, Uri.parse("tel:+49 30 1234"));
            case 2:
                return new Intent(Intent.ACTION_VIEW, Uri.parse("geo:52.52,13.40?q=Berlin"));
            case 3:
                return new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:a@example.com?subject=Hi%20there"))
                        .putExtra(Intent.EXTRA_EMAIL, new String[] {"b@example.com"})
                        .putExtra(Intent.EXTRA_TEXT, "line1\nline2");
            case 4:
                return new Intent(Intent.ACTION_SENDTO, Uri.parse("smsto:123")).putExtra("sms_body", "Hello & bye");
            case 5:
                return Intent.createChooser(new Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_SUBJECT, "Sub").putExtra(Intent.EXTRA_TEXT, "Text 😀"), "Share");
            case 6:
                return new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, report)
                        .putExtra(Intent.EXTRA_TEXT, "with a file").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            case 7:
                return new Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=com.example"));
            default:
                final ArrayList<Uri> two = new ArrayList<>(Arrays.asList(report, image));
                return new Intent(Intent.ACTION_SEND_MULTIPLE).setType("*/*")
                        .putParcelableArrayListExtra(Intent.EXTRA_STREAM, two)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.post(this::startNext);
        }
    }

    private void startNext() {
        if (mNext == NAMES.length) {
            final String[] left = new File(getApplicationInfo().dataDir, ".minato/share").list();
            Log.i(TAG, "share folders left: " + (left == null ? "none" : String.valueOf(left.length)));
            Log.i(TAG, "done");
            return;
        }
        final int index = mNext++;
        Log.i(TAG, "start " + NAMES[index]);
        try {
            startActivityForResult(intent(index), index);
        } catch (RuntimeException e) {
            // As ActivityStarter does for a start for a result that fails, the result RESULT_CANCELED follows, which
            // goes on with the next.
            Log.i(TAG, "start " + NAMES[index] + " threw " + e.getClass().getName());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        Log.i(TAG, "result " + NAMES[requestCode] + " " + resultCode);
        mHandler.post(this::startNext);
    }
}
