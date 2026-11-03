package io.github.kisame76.minato.test.imeinsets;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * ADR-0068 step 2d's test with adjustResize: a field at the bottom of the window. The activity shows the keyboard,
 * hides it, and logs after each step the IME inset (zero or not, as its height is the device's), whether the
 * content is smaller than the window and whether the field is clear of the keyboard. It then starts PanActivity
 * (adjustPan) and that UnspecifiedActivity; the last ends with "done". On Android the fixture input method's keyboard
 * makes the inset; in Minato apk-run's insets step does.
 */
public class ResizeActivity extends Activity {
    /** The app's label, which names the app under test: the log tag android-reference.sh waits on. */
    private String TAG = "ime-insets-test";
    private static final long SETTLE_MS = 1500;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private FrameLayout mRoot;
    private EditText mEdit;
    private boolean mStarted;
    private int mFullHeight;
    private int mRootTop;
    private int mImeBottom;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        mRoot = new FrameLayout(this);
        mEdit = new EditText(this);
        mRoot.addView(mEdit, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, android.view.Gravity.BOTTOM));
        getWindow().getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
            int bottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
            if ((bottom != 0) != (mImeBottom != 0)) {
                Log.i(TAG, "ime inset " + (bottom != 0 ? "nonzero" : "zero") + ", visible "
                        + insets.isVisible(WindowInsets.Type.ime()));
            }
            mImeBottom = bottom;
            return view.onApplyWindowInsets(insets);
        });
        setContentView(mRoot);
        // The content's full size and place, taken at the first layout: with adjustResize Android shows the keyboard by
        // itself once the window has the focus and a field has the focus, as soon as the input method is up, so a
        // sample after the focus is of a window that is resized already on a fast start.
        mRoot.getViewTreeObserver().addOnGlobalLayoutListener(
                new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        if (mRoot.getHeight() == 0) {
                            return;
                        }
                        mRoot.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                        mFullHeight = mRoot.getHeight();
                        int[] at = new int[2];
                        mRoot.getLocationInWindow(at);
                        mRootTop = at[1];
                    }
                });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mEdit.requestFocus();
        mHandler.postDelayed(() -> {
            getSystemService(InputMethodManager.class).showSoftInput(mEdit, 0);
            mHandler.postDelayed(this::afterShow, SETTLE_MS);
        }, SETTLE_MS);
    }

    private void afterShow() {
        report("shown");
        getSystemService(InputMethodManager.class).hideSoftInputFromWindow(mEdit.getWindowToken(), 0);
        mHandler.postDelayed(() -> {
            report("hidden");
            next();
        }, SETTLE_MS);
    }

    /** The activity after this one, or none: then the run is done. */
    Class<? extends Activity> successor() {
        return PanActivity.class;
    }

    String mode() {
        return "resize";
    }

    private void next() {
        Class<? extends Activity> next = successor();
        if (next == null) {
            Log.i(TAG, "done");
            return;
        }
        startActivity(new android.content.Intent(this, next));
        finish();
    }

    private void report(String step) {
        int[] field = new int[2];
        mEdit.getLocationOnScreen(field);
        final int fieldBottom = field[1] + mEdit.getHeight();
        int[] root = new int[2];
        mRoot.getLocationOnScreen(root);
        int[] inWindow = new int[2];
        mRoot.getLocationInWindow(inWindow);
        final View decor = getWindow().getDecorView();
        int[] decorAt = new int[2];
        decor.getLocationOnScreen(decorAt);
        // The keyboard's top in the window: the decor's bottom less the IME inset, so that the system bars, which
        // differ between devices, do not enter the line.
        final boolean clear = fieldBottom <= decorAt[1] + decor.getHeight() - mImeBottom;
        Log.i(TAG, mode() + " " + step + ": content "
                + (mRoot.getHeight() < mFullHeight ? "resized" : "full") + ", ime inset "
                + (mImeBottom != 0 ? "nonzero" : "zero")
                + (mImeBottom != 0 ? ", content " + (inWindow[1] < mRootTop ? "panned up" : "not panned") : "")
                + (mImeBottom != 0 ? ", field " + (clear ? "clear of" : "covered by") + " the keyboard" : "")
                + ", " + (fieldBottom <= root[1] + mRoot.getHeight() ? "inside" : "outside") + " the content");
    }
}
