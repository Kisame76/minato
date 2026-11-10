package io.github.kisame76.minato.test.imeoutside;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * A tap outside the text field while the keyboard is up, for each kind of field the engines and frameworks use (as
 * their open sources describe them, none of their classes), over a game view that takes every touch and no focus:
 * <ul>
 * <li>"plain": an EditText in the layout;
 * <li>"cocos": an EditText laid over the game, as Cocos2d-x's EditBox;
 * <li>"godot": a hidden one-pixel EditText that the engine focuses, as Godot's GodotEditText;
 * <li>"flutter": a view whose connection is a BaseInputConnection, as Flutter's TextInputPlugin;
 * <li>"sdl": a view that asks for no extract UI and is a text editor, as SDL2's DummyEdit.
 * </ul>
 * Each field takes the focus and asks for the keyboard; once its ime inset is there it logs "<kind> shown", and the
 * driver taps at a fifth of the window's height, in its middle, away from every field. Two seconds after the game's
 * first touch or the keyboard's hide, whichever comes first (or a minute without either), it logs "<kind> after the
 * tap: keyboard <up>, game touched <yes>, focus <kept>", hides the keyboard and goes on. The run ends with "done".
 */
public class OutsideActivity extends Activity {
    private static final String TAG = "ime-outside-test";
    private static final String[] KINDS = {"plain", "cocos", "godot", "flutter", "sdl"};
    /** How long a field waits for the tap at most: room for a test runner's start under load in Minato. */
    private static final long WAIT_MS = 60000;
    /** From the tap's first sign to the field's line. */
    private static final long SETTLE_MS = 2000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private FrameLayout mRoot;
    private boolean mImeUp;
    private boolean mTouched;
    private boolean mStarted;
    private int mAt = -1;
    private View mField;

    /** The game under the fields: every touch, no focus. */
    private final class GameView extends View {
        GameView(Context context) {
            super(context);
            setFocusable(false);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                mTouched = true;
            }
            return true;
        }
    }

    /** A view that is a text editor of its own: Flutter's (extract UI allowed) or SDL2's (none). */
    private static final class EditorView extends View {
        private final boolean mNoExtract;

        EditorView(Context context, boolean noExtract) {
            super(context);
            mNoExtract = noExtract;
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = mNoExtract ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                    : InputType.TYPE_CLASS_TEXT;
            info.imeOptions = EditorInfo.IME_ACTION_DONE
                    | (mNoExtract ? EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN : 0);
            return new BaseInputConnection(this, true);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mRoot = new FrameLayout(this);
        mRoot.addView(new GameView(this), new FrameLayout.LayoutParams(-1, -1));
        mRoot.setOnApplyWindowInsetsListener((view, insets) -> {
            mImeUp = insets.isVisible(WindowInsets.Type.ime());
            return view.onApplyWindowInsets(insets);
        });
        setContentView(mRoot);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            Log.i(TAG, "ready");
            mHandler.postDelayed(this::next, 1000);
        }
    }

    private void next() {
        if (mField != null) {
            imm().hideSoftInputFromWindow(mField.getWindowToken(), 0);
            mRoot.removeView(mField);
            mField = null;
        }
        mAt++;
        if (mAt == KINDS.length) {
            mHandler.postDelayed(() -> Log.i(TAG, "done"), 1000);
            return;
        }
        mHandler.postDelayed(this::show, 1500);
    }

    private void show() {
        final String kind = KINDS[mAt];
        // Every field in the window's lower middle, clear of the tap at a fifth of its height.
        final FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER);
        switch (kind) {
            case "plain":
            case "cocos": {
                final EditText edit = new EditText(this);
                edit.setText(kind);
                mField = edit;
                break;
            }
            case "godot": {
                final EditText edit = new EditText(this);
                edit.setAlpha(0f);
                mField = edit;
                params.width = 1;
                params.height = 1;
                break;
            }
            default:
                mField = new EditorView(this, kind.equals("sdl"));
                params.height = 150;
                break;
        }
        mRoot.addView(mField, params);
        mField.requestFocus();
        mTouched = false;
        mField.post(() -> imm().showSoftInput(mField, 0));
        waitForKeyboard(kind, 0);
    }

    private void waitForKeyboard(String kind, int tries) {
        if (mImeUp) {
            Log.i(TAG, kind + " shown");
            waitForTap(kind, SystemClock.uptimeMillis());
        } else if (tries < 40) {
            mHandler.postDelayed(() -> waitForKeyboard(kind, tries + 1), 250);
        } else {
            Log.i(TAG, kind + " no keyboard");
            next();
        }
    }

    /** The game's first touch or the keyboard's hide, then the line SETTLE_MS later; WAIT_MS at most. */
    private void waitForTap(String kind, long since) {
        if (mTouched || !mImeUp || SystemClock.uptimeMillis() - since >= WAIT_MS) {
            mHandler.postDelayed(() -> after(kind), SETTLE_MS);
        } else {
            mHandler.postDelayed(() -> waitForTap(kind, since), 100);
        }
    }

    private void after(String kind) {
        Log.i(TAG, kind + " after the tap: keyboard " + mImeUp + ", game touched " + mTouched + ", focus "
                + mField.hasFocus());
        next();
    }

    private InputMethodManager imm() {
        return getSystemService(InputMethodManager.class);
    }
}
