package io.github.kisame76.minato.test.imesurrounding;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/**
 * ADR-0068's text bar for engines: one view as SDL2's DummyEdit, which Ren'Py and other SDL apps show the keyboard
 * for. Its connection, as SDLInputConnection, is a BaseInputConnection with an editable of its own: getExtractedText
 * gives null, and the view never calls updateExtractedText or updateSelection. It logs the editable after each edit of
 * the host's text steps, and ends with "done" after the last one. Its twin ime-surrounding-test-return hides the
 * keyboard at Enter as SDL2 does with SDL_HINT_RETURN_KEY_HIDES_IME (SDLInputConnection's ENTER, SDL_StopTextInput,
 * hideSoftInputFromWindow), as Ren'Py's name field does at Return, and ends with "done" 2 s later. Its twin
 * ime-surrounding-test-delete waits for one edit only, the keyboard's delete key in the empty field, which Android's
 * keyboard still sends as deleteSurroundingText(1, 0), and ends with "done" 1 s after it. Its twin
 * ime-surrounding-test-hide hides the keyboard by itself (hideSoftInputFromWindow) 15 s after its showSoftInput, as an
 * SDL game does when it ends its text input, and ends with "done" 2 s later: the 15 s give a check on the iPhone time
 * to look at the keyboard while it is up.
 */
public class SurroundingActivity extends Activity {
    static final String RETURN_PACKAGE = "io.github.kisame76.minato.test.imesurroundingreturn";
    static final String RETURN_TAG = "ime-surrounding-test-return";
    static final String DELETE_PACKAGE = "io.github.kisame76.minato.test.imesurroundingdelete";
    static final String DELETE_TAG = "ime-surrounding-test-delete";
    static final String HIDE_PACKAGE = "io.github.kisame76.minato.test.imesurroundinghide";
    static final String HIDE_TAG = "ime-surrounding-test-hide";
    /** The twin that hides the keyboard by itself, and how long after its showSoftInput. */
    private boolean mHides;
    private boolean mHidden;
    private static final long HIDE_AFTER_MS = 15000;
    private int mEditsWanted = 3;
    private static final long TIMEOUT_MS = 12000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private DummyEdit mEdit;
    private boolean mStarted;
    private boolean mDone;
    /** The twin that hides the keyboard at Enter, and its tag. */
    private boolean mReturns;
    private boolean mReturned;
    private String mTag = "ime-surrounding-test";
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

        /** SDL2's ENTER with SDL_HINT_RETURN_KEY_HIDES_IME: the twin hides the keyboard, as SDL_StopTextInput does. */
        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (!mReturns || event.getKeyCode() != KeyEvent.KEYCODE_ENTER) {
                return super.sendKeyEvent(event);
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN && !mReturned) {
                mReturned = true;
                boolean hidden = getSystemService(InputMethodManager.class)
                        .hideSoftInputFromWindow(mEdit.getWindowToken(), 0);
                Log.i(mTag, "return: text \"" + getEditable() + "\", hideSoftInputFromWindow " + hidden);
                mHandler.postDelayed(SurroundingActivity.this::finishRun, 2000);
            }
            return true;
        }

        private boolean edited(String call, boolean result) {
            Log.i(mTag, "call " + call + " -> " + result + ", text \"" + getEditable() + "\"");
            if (++mEdits == mEditsWanted) {
                mHandler.postDelayed(SurroundingActivity.this::finishRun, mEditsWanted == 1 ? 1000 : 500);
            }
            return result;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mReturns = RETURN_PACKAGE.equals(getPackageName());
        if (mReturns) {
            mTag = RETURN_TAG;
        }
        if (DELETE_PACKAGE.equals(getPackageName())) {
            mTag = DELETE_TAG;
            mEditsWanted = 1;
        }
        mHides = HIDE_PACKAGE.equals(getPackageName());
        if (mHides) {
            mTag = HIDE_TAG;
        }
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
        Log.i(mTag, "showSoftInput " + getSystemService(InputMethodManager.class).showSoftInput(mEdit, 0));
        if (mHides) {
            mHandler.postDelayed(() -> {
                mHidden = getSystemService(InputMethodManager.class)
                        .hideSoftInputFromWindow(mEdit.getWindowToken(), 0);
                Log.i(mTag, "hide: hideSoftInputFromWindow " + mHidden);
                mHandler.postDelayed(this::finishRun, 2000);
            }, HIDE_AFTER_MS);
        }
        mHandler.postDelayed(this::finishRun, mHides ? HIDE_AFTER_MS + 5000 : TIMEOUT_MS);
    }

    private void finishRun() {
        if (mDone) {
            return;
        }
        mDone = true;
        Log.i(mTag, mEdits == mEditsWanted || mReturned || mHidden ? "done" : "timeout");
    }
}
