package io.github.kisame76.minato.test.nullbackgrounddecor;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * Hypothesis 2 variant of the black-screen sweep (black-app-probe-plan): a window whose theme sets
 * android:windowBackground="@null", so the decor posts NO background of its own; the content view alone provides the
 * pixels. Drawn in software (hardwareAccelerated="false"), the content fills the window with one opaque colour and
 * posts it ONCE. shown() must present that opaque content even though the window has no background drawable; a window
 * that treated the missing background as nothing to show would go black. With MINATO_WINDOW_TRACE the host logs
 * shown()'s decision per post.
 */
public class NullBackgroundActivity extends Activity {
    private String mTag;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(mTag, "onCreate");
        final View view = new View(this);
        view.setBackgroundColor(0xFFCC3344);
        setContentView(view);
        view.getViewTreeObserver().addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
            @Override
            public void onDraw() {
                if (mDone) {
                    return;
                }
                mDone = true;
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
