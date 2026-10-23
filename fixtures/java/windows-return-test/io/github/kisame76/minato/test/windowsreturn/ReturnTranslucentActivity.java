package io.github.kisame76.minato.test.windowsreturn;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import java.util.ArrayList;
import java.util.List;

/**
 * The translucent activity of windows-return-test (ADR-0041 gap 4 (b5a)), on Theme.Translucent.NoTitleBar above the
 * launcher activity, its dialog and popup: a SurfaceView filled by lockCanvas, whose callbacks it keeps for one line
 * after the return; "ready" a moment after its first focus, which the reference's HOME waits for; after the return,
 * once it has the focus again, the windows as the app sees them, then its finish.
 */
public class ReturnTranslucentActivity extends Activity {
    private static final int SURFACE_SIZE = 300;
    private final List<String> mSurface = new ArrayList<>();
    private int mFocusCount;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Canvas canvas = holder.lockCanvas();
                if (canvas != null) {
                    canvas.drawColor(Color.BLUE);
                    holder.unlockCanvasAndPost(canvas);
                }
                mSurface.add("created, canvas " + (canvas != null));
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                mSurface.add("changed " + width + "x" + height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                mSurface.add("destroyed");
            }
        });
        root.addView(surface, new FrameLayout.LayoutParams(SURFACE_SIZE, SURFACE_SIZE, Gravity.TOP | Gravity.START));
        setContentView(root);
        ReturnActivity.sInstance.watchFocus("translucent", root);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        ReturnActivity.log("translucent onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        ReturnActivity.log("translucent onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        ReturnActivity.log("translucent onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        ReturnActivity.log("translucent onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        ReturnActivity.log("translucent onStop");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) {
            return;
        }
        final ReturnActivity main = ReturnActivity.sInstance;
        final int count = ++mFocusCount;
        main.mHandler.postDelayed(() -> {
            main.logFocus();
            if (count == 1) {
                ReturnActivity.log("before HOME: " + main.windows());
                ReturnActivity.log("ready");
            } else if (count == 2) {
                ReturnActivity.log("after the return: " + main.windows());
                ReturnActivity.log("translucent surface: " + String.join(", ", mSurface));
                finish();
            }
        }, ReturnActivity.SETTLE_MS);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ReturnActivity.log("translucent destroyed");
        ReturnActivity.sInstance.afterTranslucent();
    }
}
