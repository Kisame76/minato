package io.github.kisame76.minato.test.translucentdecor;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * Hypothesis 2 of the black-screen sweep (black-app-probe-plan): a non-opaque (translucent) main window. The theme is
 * Theme.Translucent.NoTitleBar and the activity is drawn in software (hardwareAccelerated="false"), so the decor layer
 * is non-opaque. The activity fills its one view with fillColor() and posts it ONCE, as a static software UI does, and
 * never reposts. shown() must present an OPAQUE fill on a non-opaque layer (it has visible pixels); a fully TRANSPARENT
 * fill is the known "nothing to show" case and must be hidden as transparent. The subclasses give each fill, so a
 * wrong hide of the opaque fill is the black bug. With MINATO_WINDOW_TRACE the host logs shown()'s decision per post.
 */
public abstract class TranslucentDecorActivity extends Activity {
    private String mTag;
    private boolean mDone;

    /** The colour this variant fills the window with: an opaque or a fully transparent ARGB. */
    protected abstract int fillColor();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(mTag, "onCreate");
        final View view = new View(this);
        view.setBackgroundColor(fillColor());
        setContentView(view);
        view.getViewTreeObserver().addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
            @Override
            public void onDraw() {
                if (mDone) {
                    return;
                }
                mDone = true;
                // After the first (and only) post, on the main queue so it follows the draw that produced it.
                view.post(() -> Log.i(mTag, "done"));
            }
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(mTag, "onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(mTag, "onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(mTag, "focus " + hasFocus);
    }
}
