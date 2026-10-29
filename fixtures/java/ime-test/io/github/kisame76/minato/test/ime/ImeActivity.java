package io.github.kisame76.minato.test.ime;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.util.Log;
import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * ADR-0068 step 2a's test: an EditText whose InputConnection logs each call an input method makes, with the arguments
 * and the text, selection and composing span after it, beside the EditorInfo it was served with and the input method
 * manager's answers. On Android the fixture input method (FixtureInputMethod) makes the calls; in Minato apk-run's text
 * steps do. The test ends with "done" after the last key's release, or with "timeout" and "done" after ten seconds.
 */
public class ImeActivity extends Activity {
    static final String PACKAGE = "io.github.kisame76.minato.test.ime";
    private static final String TAG = "ime-test";
    private static final long TIMEOUT_MS = 10000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private LoggingEditText mEdit;
    private boolean mStarted;
    private boolean mDone;
    private String mLastEditor;

    /** The field: it hands the input method a connection that logs, around the one an EditText makes. */
    private class LoggingEditText extends EditText {
        LoggingEditText(Context context) {
            super(context);
            setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_GO);
            setSingleLine(true);
            setHint("hint");
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            InputConnection target = super.onCreateInputConnection(outAttrs);
            // Once per distinct editor: how often the connection is made again differs by input method.
            String editor = "editor: inputType 0x" + Integer.toHexString(outAttrs.inputType) + ", imeOptions 0x"
                    + Integer.toHexString(outAttrs.imeOptions) + ", capsMode " + outAttrs.initialCapsMode
                    + ", selection " + outAttrs.initialSelStart + "," + outAttrs.initialSelEnd + ", hint \""
                    + outAttrs.hintText + "\", package " + outAttrs.packageName + ", fieldId " + outAttrs.fieldId;
            if (!editor.equals(mLastEditor)) {
                mLastEditor = editor;
                Log.i(TAG, editor);
            }
            return target == null ? null : new LoggingConnection(target);
        }
    }

    private class LoggingConnection extends InputConnectionWrapper {
        LoggingConnection(InputConnection target) {
            super(target, true);
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            boolean result = super.setComposingText(text, newCursorPosition);
            logged("setComposingText \"" + text + "\" " + newCursorPosition, result);
            return result;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            boolean result = super.commitText(text, newCursorPosition);
            logged("commitText \"" + text + "\" " + newCursorPosition, result);
            return result;
        }

        @Override
        public boolean finishComposingText() {
            // Not logged: the input method manager makes this call itself, at times of its own choosing.
            return super.finishComposingText();
        }

        @Override
        public boolean setComposingRegion(int start, int end) {
            boolean result = super.setComposingRegion(start, end);
            logged("setComposingRegion " + start + "," + end, result);
            return result;
        }

        @Override
        public boolean setSelection(int start, int end) {
            boolean result = super.setSelection(start, end);
            logged("setSelection " + start + "," + end, result);
            return result;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            boolean result = super.deleteSurroundingText(beforeLength, afterLength);
            logged("deleteSurroundingText " + beforeLength + "," + afterLength, result);
            return result;
        }

        @Override
        public boolean performEditorAction(int editorAction) {
            boolean result = super.performEditorAction(editorAction);
            logged("performEditorAction " + editorAction, result);
            return result;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            boolean result = super.sendKeyEvent(event);
            logged("sendKeyEvent " + KeyEvent.keyCodeToString(event.getKeyCode()) + " "
                    + (event.getAction() == KeyEvent.ACTION_DOWN ? "down" : "up"), result);
            if (event.getKeyCode() == KeyEvent.KEYCODE_DEL && event.getAction() == KeyEvent.ACTION_UP) {
                mHandler.post(ImeActivity.this::finishRun);
            }
            return result;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mEdit = new LoggingEditText(this);
        mEdit.setOnEditorActionListener((view, actionId, event) -> {
            Log.i(TAG, "editor action " + actionId + ", event " + (event == null ? "none" : "key"));
            return false;
        });
        FrameLayout root = new FrameLayout(this);
        // The field fills the window, so that a tap anywhere is a tap on it.
        root.addView(mEdit, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mEdit.requestFocus();
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        Log.i(TAG, "isActive " + imm.isActive(mEdit) + ", isAcceptingText " + imm.isAcceptingText());
        Log.i(TAG, "showSoftInput " + imm.showSoftInput(mEdit, 0));
        mHandler.postDelayed(() -> {
            if (!mDone) {
                Log.i(TAG, "timeout");
                finishRun();
            }
        }, TIMEOUT_MS);
    }

    private void logged(String call, boolean result) {
        Editable text = mEdit.getText();
        Log.i(TAG, "call " + call + " -> " + result);
        Log.i(TAG, "  text \"" + text + "\", selection " + mEdit.getSelectionStart() + "," + mEdit.getSelectionEnd()
                + ", composing " + BaseInputConnection.getComposingSpanStart(text) + ","
                + BaseInputConnection.getComposingSpanEnd(text));
    }

    private void finishRun() {
        if (mDone) {
            return;
        }
        mDone = true;
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        Log.i(TAG, "isAcceptingText " + imm.isAcceptingText());
        Log.i(TAG, "final text \"" + mEdit.getText() + "\"");
        Log.i(TAG, "done");
    }
}
