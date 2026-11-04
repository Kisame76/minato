package io.github.kisame76.minato.test.imeextract;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * ADR-0068's test of the host's text bar: a note field (multi-line, with text and a selection at its end) and below it
 * a password field. Each field's InputConnection logs the calls of an input method that shows the field's text:
 * getExtractedText (flags 0), and the edits after it, with the text, selection and composing span
 * after each. After the note field's last edit the password field takes the focus. The test ends with "done".
 */
public class ExtractActivity extends Activity {
    static final String PACKAGE = "io.github.kisame76.minato.test.imeextract";
    private static final String TAG = "ime-extract-test";
    private static final long TIMEOUT_MS = 12000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private LoggingEditText mNote;
    private LoggingEditText mPassword;
    private boolean mStarted;
    private boolean mDone;

    /** A field whose connection logs, around the one an EditText makes. */
    private class LoggingEditText extends EditText {
        private final String mName;
        private final java.util.concurrent.atomic.AtomicBoolean mExtractLogged =
                new java.util.concurrent.atomic.AtomicBoolean();

        LoggingEditText(Context context, String name) {
            super(context);
            mName = name;
            setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            final InputConnection target = super.onCreateInputConnection(outAttrs);
            return target == null ? null : new LoggingConnection(target, this, mName, mExtractLogged);
        }
    }

    private class LoggingConnection extends InputConnectionWrapper {
        private final EditText mField;
        private final String mName;
        private final java.util.concurrent.atomic.AtomicBoolean mLogged;

        LoggingConnection(InputConnection target, EditText field, String name,
                java.util.concurrent.atomic.AtomicBoolean logged) {
            super(target, true);
            mField = field;
            mName = name;
            mLogged = logged;
        }

        @Override
        public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
            final ExtractedText result = super.getExtractedText(request, flags);
            // The first call, once per field: how often a connection is made again differs by input method (as
            // ime-test's editor line), and the keyboard reads the text again after the view's updates.
            if (!mLogged.getAndSet(true)) {
                Log.i(TAG, mName + ": call getExtractedText hintMaxChars " + request.hintMaxChars + " token "
                        + request.token + " flags " + flags + " -> " + (result == null || result.text == null ? "none"
                        : "length " + result.text.length() + ", selection " + result.selectionStart + ","
                        + result.selectionEnd));
            }
            return result;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            final boolean result = super.commitText(text, newCursorPosition);
            logged("commitText \"" + text + "\" " + newCursorPosition, result);
            return result;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            final boolean result = super.setComposingText(text, newCursorPosition);
            logged("setComposingText \"" + text + "\" " + newCursorPosition, result);
            return result;
        }

        @Override
        public boolean finishComposingText() {
            // The manager makes this call itself, at times of its own choosing: not logged.
            return super.finishComposingText();
        }

        @Override
        public boolean setSelection(int start, int end) {
            final boolean result = super.setSelection(start, end);
            logged("setSelection " + start + "," + end, result);
            // The last edit: the password field takes the focus a moment later.
            mHandler.postDelayed(ExtractActivity.this::focusPassword, 300);
            return result;
        }

        private void logged(String call, boolean result) {
            final Editable text = mField.getText();
            Log.i(TAG, mName + ": call " + call + " -> " + result);
            Log.i(TAG, "  text \"" + text + "\", selection " + mField.getSelectionStart() + ","
                    + mField.getSelectionEnd() + ", composing " + BaseInputConnection.getComposingSpanStart(text)
                    + "," + BaseInputConnection.getComposingSpanEnd(text));
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mNote = new LoggingEditText(this, "note");
        mNote.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        mNote.setHint("note");
        mNote.setText("héllo 😀");
        mNote.setSelection(mNote.length());
        mPassword = new LoggingEditText(this, "password");
        mPassword.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        mPassword.setText("secret");
        mPassword.setSelection(mPassword.length());
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.addView(mNote, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        column.addView(mPassword, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(column);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mNote.requestFocus();
        final InputMethodManager imm = getSystemService(InputMethodManager.class);
        Log.i(TAG, "showSoftInput " + imm.showSoftInput(mNote, 0));
        mHandler.postDelayed(() -> {
            if (!mDone) {
                Log.i(TAG, "timeout");
                finishRun();
            }
        }, TIMEOUT_MS);
    }

    private void focusPassword() {
        mPassword.requestFocus();
        final InputMethodManager imm = getSystemService(InputMethodManager.class);
        Log.i(TAG, "showSoftInput " + imm.showSoftInput(mPassword, 0));
        mHandler.postDelayed(this::finishRun, 1500);
    }

    private void finishRun() {
        if (mDone) {
            return;
        }
        mDone = true;
        Log.i(TAG, "final note \"" + mNote.getText() + "\"");
        Log.i(TAG, "final password length " + mPassword.length());
        Log.i(TAG, "done");
    }
}
