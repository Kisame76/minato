package io.github.kisame76.minato.test.imesurrounding;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/**
 * ADR-0068's text bar for engines: one view as SDL2's DummyEdit, which Ren'Py and other SDL apps show the keyboard
 * for. Its connection, as SDLInputConnection, is a BaseInputConnection with an editable of its own: getExtractedText
 * gives null, and the view never calls updateExtractedText or updateSelection. It logs the editable after each edit of
 * the host's text steps, and ends with "done" after the last one.
 */
public class SurroundingActivity extends Activity {
    private static final String TAG = "ime-surrounding-test";
    private static final int EDITS = 3;
    private static final long TIMEOUT_MS = 12000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private DummyEdit mEdit;
    private boolean mStarted;
    private boolean mDone;
    private int mEdits;

    /** As SDL2's DummyEdit: focusable, a text editor, and its own connection. */
    private class DummyEdit extends View {
        DummyEdit(Context context) {
            super(context);
            setFocusableInTouchMode(true);
            setFocusable(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            return new SdlStyleConnection(this);
        }
    }

    /** As SDLInputConnection: a full editor's BaseInputConnection that logs the editable after each edit. */
    private class SdlStyleConnection extends BaseInputConnection {
        SdlStyleConnection(View view) {
            super(view, true);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            return edited("commitText \"" + text + "\"", super.commitText(text, newCursorPosition));
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            return edited("deleteSurroundingText " + beforeLength + "," + afterLength,
                    super.deleteSurroundingText(beforeLength, afterLength));
        }

        private boolean edited(String call, boolean result) {
            Log.i(TAG, "call " + call + " -> " + result + ", text \"" + getEditable() + "\"");
            if (++mEdits == EDITS) {
                mHandler.postDelayed(SurroundingActivity.this::finishRun, 500);
            }
            return result;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mEdit = new DummyEdit(this);
        setContentView(mEdit);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mEdit.requestFocus();
        Log.i(TAG, "showSoftInput " + getSystemService(InputMethodManager.class).showSoftInput(mEdit, 0));
        mHandler.postDelayed(this::finishRun, TIMEOUT_MS);
    }

    private void finishRun() {
        if (mDone) {
            return;
        }
        mDone = true;
        Log.i(TAG, mEdits == EDITS ? "done" : "timeout");
    }
}
