package io.github.kisame76.minato.test.clipboard;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * The clipboard as an app uses it (the Unity test APK probe: UnityPlayer's constructor asks for the service): the primary clip
 * set, read and cleared, its description and the text check, the clip an app gets back being a copy, and the
 * listeners told of a change. Android gives the clipboard to the app that has the focus only, so the test runs when the
 * launcher activity first has the window focus.
 */
public class ClipboardActivity extends Activity {
    static final String TAG = "clipboard-test";
    private static final int SETTLE_MS = 400;

    private ClipboardManager mClipboard;
    private final AtomicInteger mChanges = new AtomicInteger();
    private final ClipboardManager.OnPrimaryClipChangedListener mListener = mChanges::incrementAndGet;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private boolean mStarted;

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            // A thread of its own: the changes are told on the main thread.
            new Thread(this::first, "clipboard-test").start();
        }
    }

    @Override
    protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        mClipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        log("the service: " + (mClipboard == null ? "null" : mClipboard.getClass().getName()));
        log("the same service again: " + (getSystemService(Context.CLIPBOARD_SERVICE) == mClipboard));
        log("by class: " + (getSystemService(ClipboardManager.class) == mClipboard));
    }

    private static String describe(ClipData clip) {
        if (clip == null) {
            return "null";
        }
        ClipDescription description = clip.getDescription();
        StringBuilder mimes = new StringBuilder();
        for (int i = 0; i < description.getMimeTypeCount(); i++) {
            mimes.append(i == 0 ? "" : ",").append(description.getMimeType(i));
        }
        ClipData.Item item = clip.getItemAt(0);
        return "label [" + description.getLabel() + "], items " + clip.getItemCount() + ", mime " + mimes
                + ", text [" + item.getText() + "], html [" + item.getHtmlText() + "], uri [" + item.getUri()
                + "], intent [" + (item.getIntent() == null ? null : item.getIntent().getAction()) + "]";
    }

    private static void settle(int milliseconds) {
        try {
            Thread.sleep(milliseconds);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void first() {
        mClipboard.clearPrimaryClip();
        mClipboard.addPrimaryClipChangedListener(mListener);
        mChanges.set(0);
        log("cleared: has " + mClipboard.hasPrimaryClip() + ", clip " + describe(mClipboard.getPrimaryClip())
                + ", description " + mClipboard.getPrimaryClipDescription() + ", has text " + mClipboard.hasText());
        mClipboard.setPrimaryClip(ClipData.newPlainText("note", "hello"));
        settle(SETTLE_MS);
        log("changes after the first set: " + mChanges.get());

        log("plain text: has " + mClipboard.hasPrimaryClip() + ", clip " + describe(mClipboard.getPrimaryClip())
                + ", has text " + mClipboard.hasText() + ", text " + mClipboard.getText());
        log("classification of the text: " + mClipboard.getPrimaryClipDescription().getClassificationStatus());
        log("its description: " + mClipboard.getPrimaryClipDescription().getLabel() + ", "
                + mClipboard.getPrimaryClipDescription().getMimeTypeCount() + " type, text "
                + mClipboard.getPrimaryClipDescription().hasMimeType(ClipDescription.MIMETYPE_TEXT_PLAIN));
        ClipData copy = mClipboard.getPrimaryClip();
        copy.addItem(new ClipData.Item("second"));
        log("a copy changed: the clipboard has " + mClipboard.getPrimaryClip().getItemCount() + " item, the copy "
                + copy.getItemCount());
        settle(SETTLE_MS);
        log("changes after the reads: " + mChanges.get());

        mChanges.set(0);
        mClipboard.setPrimaryClip(ClipData.newHtmlText("page", "plain", "<b>bold</b>"));
        log("html: " + describe(mClipboard.getPrimaryClip()) + ", has text " + mClipboard.hasText());
        mClipboard.setPrimaryClip(ClipData.newRawUri("link", Uri.parse("content://io.example/item/1")));
        log("uri: " + describe(mClipboard.getPrimaryClip()) + ", has text " + mClipboard.hasText()
                + ", classification " + mClipboard.getPrimaryClipDescription().getClassificationStatus());
        mClipboard.setPrimaryClip(ClipData.newIntent("go", new Intent("io.example.GO")));
        log("intent: " + describe(mClipboard.getPrimaryClip()) + ", has text " + mClipboard.hasText()
                + ", classification " + mClipboard.getPrimaryClipDescription().getClassificationStatus());
        mClipboard.setPrimaryClip(ClipData.newPlainText("empty", ""));
        log("an empty text: has " + mClipboard.hasPrimaryClip() + ", has text " + mClipboard.hasText());
        log("a null clip: " + outcome(() -> {
            mClipboard.setPrimaryClip(null);
            return "fine";
        }));
        settle(SETTLE_MS);
        log("changes: " + mChanges.get());
        mChanges.set(0);
        mClipboard.clearPrimaryClip();
        mClipboard.clearPrimaryClip();
        settle(SETTLE_MS);
        log("cleared twice: " + mChanges.get() + " change, has " + mClipboard.hasPrimaryClip());
        mChanges.set(0);
        mClipboard.removePrimaryClipChangedListener(mListener);
        mClipboard.setPrimaryClip(ClipData.newPlainText("again", "x"));
        settle(SETTLE_MS);
        log("after the listener was removed: " + mChanges.get() + " change, has " + mClipboard.hasPrimaryClip());
        mClipboard.clearPrimaryClip();
        log("done");
    }
}
