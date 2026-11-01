package io.github.kisame76.minato.test.imehide;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.LinearLayout;

/**
 * ADR-0068, the keyboard reopen bug: two engine-style fields, each of which tracks the soft keyboard itself and calls
 * showSoftInput on a tap only when it believes the keyboard is hidden. "Key" resets its belief in onKeyPreIme(BACK),
 * as engines' input dialogs do; "Inset" follows the IME inset (WindowInsets.isVisible(ime())). Each logs its belief,
 * every showSoftInput and its answer, every BACK its onKeyPreIme saw, and every change of the inset. The test taps a
 * field (the keyboard shows), hides the keyboard (BACK, which is what the navigation bar's hide key sends too: both
 * measured the same), and taps the field again. "done" follows 16 seconds after the first focus.
 */
public class HideActivity extends Activity {
    /** The app's label, which names the app under test: the log tag android-reference.sh waits on. */
    private static String TAG = "ime-hide-test";
    private boolean mImeVisible;
    private boolean mReady;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** A field that is served by the input method and tracks the keyboard its own way. */
    private final class Field extends View {
        private final String mName;
        private final boolean mByInset;
        private boolean mBelievesShown;

        Field(Context context, String name, boolean byInset) {
            super(context);
            mName = name;
            mByInset = byInset;
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
            outAttrs.packageName = getContext().getPackageName();
            return new BaseInputConnection(this, true);
        }

        @Override
        public boolean onKeyPreIme(int keyCode, KeyEvent event) {
            if (keyCode == KeyEvent.KEYCODE_BACK) {
                final boolean up = event.getAction() == KeyEvent.ACTION_UP;
                // The navigation bar's BACK, measured by hand on the 3-button emulator, has source 0x101 and flags 0x48
                // (FROM_SYSTEM | VIRTUAL_HARD_KEY); the script's `input keyboard keyevent` has the source only.
                log(mName + " onKeyPreIme BACK " + (up ? "up" : "down") + ", source 0x"
                        + Integer.toHexString(event.getSource()) + ", believed shown "
                        + (mByInset ? mImeVisible : mBelievesShown));
                if (up && !mByInset) {
                    mBelievesShown = false;
                }
            }
            return false;
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                requestFocusFromTouch();
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_UP) {
                final boolean believed = mByInset ? mImeVisible : mBelievesShown;
                if (believed) {
                    log(mName + " tap: believes the keyboard is shown, no showSoftInput");
                } else {
                    final boolean answer = getSystemService(InputMethodManager.class).showSoftInput(this, 0);
                    log(mName + " tap: showSoftInput " + answer);
                    mBelievesShown = true;
                }
            }
            return true;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(new Field(this, "Key", false), new LinearLayout.LayoutParams(-1, 300));
        root.addView(new Field(this, "Inset", true), new LinearLayout.LayoutParams(-1, 300));
        setContentView(root);
        getWindow().getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
            final boolean visible = insets.isVisible(WindowInsets.Type.ime());
            if (visible != mImeVisible) {
                mImeVisible = visible;
                final int bottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
                log("ime inset visible " + visible + ", bottom " + (bottom != 0 ? "nonzero" : "zero"));
            }
            return view.onApplyWindowInsets(insets);
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mReady) {
            mReady = true;
            log("ready: hide");
            // The script's taps and BACKs take about ten seconds.
            getWindow().getDecorView().postDelayed(() -> log("done"), 16000);
        }
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            log("activity BACK up");
        }
        return super.onKeyUp(keyCode, event);
    }
}
