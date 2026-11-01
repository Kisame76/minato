package io.github.kisame76.minato.test.imeextract;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;

/**
 * The fixture input method of the Android reference: a keyboard with a text bar. When ExtractActivity's fields are
 * served it asks each for its extracted text with GET_EXTRACTED_TEXT_MONITOR, as such a keyboard does, logs under the
 * device tag what it is told (the numbers that Minato's extract requests must carry), and, for the note field, plays
 * the edits that tools/ime-extract-check.sh's text steps post: a commit, a composing text, its end and a selection.
 */
public class FixtureInputMethod extends InputMethodService {
    private static final long STEP_MS = 200;
    private static final String DEVICE_TAG = "ime-extract-test-device";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mStarts;

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        if (info == null || !ExtractActivity.PACKAGE.equals(info.packageName) || restarting) {
            return;
        }
        final int field = ++mStarts;
        final InputConnection connection = getCurrentInputConnection();
        final ExtractedTextRequest request = new ExtractedTextRequest();
        request.token = 1;
        request.hintMaxChars = 4096;
        final ExtractedText extracted = connection.getExtractedText(request, InputConnection.GET_EXTRACTED_TEXT_MONITOR);
        Log.i(DEVICE_TAG, "field " + field + " extracted: " + describe(extracted) + ", flags "
                + ((info.inputType & EditorInfo.TYPE_MASK_VARIATION) == EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
                        ? "password" : "plain"));
        if (field != 1) {
            return;
        }
        final Runnable[] calls = {
            () -> connection.commitText("!", 1),
            () -> connection.setComposingText("ab", 1),
            () -> connection.finishComposingText(),
            () -> connection.setSelection(2, 4),
        };
        for (int i = 0; i < calls.length; i++) {
            mHandler.postDelayed(calls[i], STEP_MS * (i + 1));
        }
    }

    @Override
    public void onUpdateSelection(int oldSelStart, int oldSelEnd, int newSelStart, int newSelEnd,
            int candidatesStart, int candidatesEnd) {
        Log.i(DEVICE_TAG, "selection " + newSelStart + "," + newSelEnd + " composing " + candidatesStart + ","
                + candidatesEnd);
        super.onUpdateSelection(oldSelStart, oldSelEnd, newSelStart, newSelEnd, candidatesStart, candidatesEnd);
    }

    @Override
    public void onUpdateExtractedText(int token, ExtractedText text) {
        Log.i(DEVICE_TAG, "updated token " + token + ": " + describe(text) + ", partial " + (text != null
                ? text.partialStartOffset + "," + text.partialEndOffset : "none"));
    }

    private static String describe(ExtractedText text) {
        if (text == null || text.text == null) {
            return "none";
        }
        return "length " + text.text.length() + " selection " + (text.selectionStart + text.startOffset) + ","
                + (text.selectionEnd + text.startOffset);
    }
}
