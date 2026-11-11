package io.github.kisame76.minato.test.surfaceviewdecor;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;

/**
 * Hypothesis 3 of the black-screen sweep (black-app-probe-plan): a SurfaceView over a software decor, so the window
 * has two producers. The decor is an opaque FrameLayout drawn in software (hardwareAccelerated="false"); a child
 * SurfaceView fills a top banner, clear of the pixel check's 3x3 sample grid (every grid row is well below 300 px),
 * and posts its OWN first frame once (CPU in the software variant, GL in the gl variant) and never reposts. The decor
 * must stay shown at every sample point: a legitimate live second producer must not hide the decor the way the
 * dropped start-probe did (hypothesis 1). The banner keeps the SurfaceView off the sample grid because a GL
 * SurfaceView renders through a Metal drawable, which the CPU window dump cannot capture to compare in its region.
 * With MINATO_WINDOW_TRACE the host logs shown()'s decision and who is above whom.
 */
public abstract class SurfaceDecorActivity extends Activity {
    private String mTag;
    private boolean mProduced;
    private SurfaceView mView;

    /** Posts the SurfaceView's single frame on its surface; the producer stays connected afterwards. */
    protected abstract void produce(SurfaceHolder holder);

    protected void say(String line) {
        Log.i(mTag, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        Log.i(mTag, "onCreate");
        final FrameLayout decor = new FrameLayout(this);
        decor.setBackgroundColor(0xFF2244CC);  // an opaque software decor around the SurfaceView's hole
        mView = new SurfaceView(this);
        final FrameLayout.LayoutParams lp =
                new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 300, Gravity.TOP);
        decor.addView(mView, lp);
        mView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                if (mProduced) {
                    return;
                }
                mProduced = true;
                produce(holder);
                // No further post on either producer: a static two-producer window.
                mView.post(() -> Log.i(mTag, "done"));
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {}
        });
        setContentView(decor);
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
