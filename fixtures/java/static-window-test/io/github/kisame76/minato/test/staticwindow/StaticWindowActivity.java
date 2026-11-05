package io.github.kisame76.minato.test.staticwindow;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

/**
 * A static software window (the black-screen bug, ADR-0043 b5): a plain activity with
 * android:hardwareAccelerated="false" whose single view fills the window with one opaque colour and is drawn ONCE,
 * after the first layout, and then never invalidated again -- a single CPU post for the whole life, as a static menu
 * or title screen does, unlike the test's other apps which post again on a focus change, a click or an animation.
 *
 * The pixel round (tools/window-contents-check.sh) checks that the window keeps showing that one buffer. A window
 * whose "minato.window.buffers" sublayer a second producer's drawable hid, and that never posts again, would stay
 * black -- the pattern seen on the device with plain software apps. With MINATO_WINDOW_TRACE=1 (graphics/host) the
 * host logs shown()'s decision for each post, which on the device names who the second producer is.
 */
public class StaticWindowActivity extends Activity {
    private String TAG;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(TAG, "onCreate");
        final View view = new View(this);
        // One opaque colour over the whole window, drawn in software (hardwareAccelerated="false"); no visible-pixel
        // ambiguity (any_visible is yes) and no alpha, so shown() must present it on an opaque or a non-opaque layer.
        view.setBackgroundColor(0xFFCC3344);
        setContentView(view);
        view.getViewTreeObserver().addOnDrawListener(new android.view.ViewTreeObserver.OnDrawListener() {
            @Override
            public void onDraw() {
                if (mDone) {
                    return;
                }
                mDone = true;
                // After the first (and only) post, on the main queue so it follows the draw that produced it.
                view.post(() -> Log.i(TAG, "done"));
            }
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(TAG, "focus " + hasFocus);
        // Deliberately no redraw here: the window posts exactly once, as a static screen does.
    }
}
