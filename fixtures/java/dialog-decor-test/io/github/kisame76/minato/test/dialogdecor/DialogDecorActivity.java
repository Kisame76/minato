package io.github.kisame76.minato.test.dialogdecor;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * The Dialog-themed main window of the black-screen sweep (black-app-probe-plan): a launcher activity with
 * Theme.DeviceDefault.Dialog, so its window is a floating, non-fullscreen decor with a dim behind it, drawn in
 * software (hardwareAccelerated="false"). It gives the window a fixed size (a plain Dialog window wraps its content,
 * which a size-less View would collapse to nothing), fills it with one opaque colour and posts it ONCE. The floating
 * window's buffer must stay shown over the dim; a dialog-themed start window (the Unity/Mono pattern) must not go
 * black. With MINATO_WINDOW_TRACE the host logs shown()'s decision per post.
 */
public class DialogDecorActivity extends Activity {
    private String mTag;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(mTag, "onCreate");
        getWindow().setLayout(600, 600);  // a definite floating window, not a wrap-content collapse
        final View view = new View(this);
        view.setBackgroundColor(0xFFCC8822);
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
