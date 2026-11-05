package io.github.kisame76.minato.test.edgeback;

import android.app.Activity;
import android.content.Context;
import android.graphics.Rect;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.window.BackEvent;
import android.window.OnBackAnimationCallback;
import android.window.OnBackInvokedDispatcher;
import java.util.Collections;

/**
 * ADR-0041 decision 10 step b2's test of the edge back gesture. The window is one pad that logs what reaches it of
 * each touch (its down, whether moves came, its up or its cancel); an OnBackAnimationCallback logs the gesture's
 * phases (started from which edge, whether progress came, invoked or cancelled). The pad excludes a band of its left
 * edge from system gestures, 48 dp wide, from 40 % to 50 % of its height. The immersive twins hide the system bars,
 * which a swipe shows for a while (transient bars, which do not change the window's insets). Once the window has the
 * focus and its size, "ready" comes; a tap in the middle third ends the run with "done". Places, sizes and progress
 * values depend on the device, so they go under the label with "-device".
 */
public class EdgeActivity extends Activity {
    private String mTag;
    private String mDeviceTag;
    private boolean mReady;
    private boolean mDone;
    private int mProgressed;
    private float mLastProgress;

    private final OnBackAnimationCallback mBack = new OnBackAnimationCallback() {
        @Override
        public void onBackStarted(BackEvent event) {
            mProgressed = 0;
            log("back: started from the " + edge(event.getSwipeEdge()) + " edge");
            device("back: started at " + dp(event.getTouchX()) + " dp, progress " + event.getProgress());
        }

        @Override
        public void onBackProgressed(BackEvent event) {
            mProgressed++;
            mLastProgress = event.getProgress();
        }

        @Override
        public void onBackInvoked() {
            log("back: invoked, progressed " + (mProgressed > 0));
            device("back: invoked after " + mProgressed + " progress events, the last " + mLastProgress);
        }

        @Override
        public void onBackCancelled() {
            log("back: cancelled, progressed " + (mProgressed > 0));
            device("back: cancelled after " + mProgressed + " progress events, the last " + mLastProgress);
        }
    };

    /** The window's one view: it logs the touches and keeps the left edge's band for itself. */
    private class Pad extends View {
        private boolean mMoved;
        private float mDownX;

        Pad(Context context) {
            super(context);
            setBackgroundColor(0xff336699);
        }

        @Override
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            super.onLayout(changed, left, top, right, bottom);
            int height = bottom - top;
            setSystemGestureExclusionRects(Collections.singletonList(
                    new Rect(0, height * 4 / 10, Math.round(48 * density()), height * 5 / 10)));
            if (!mReady && hasWindowFocus()) {
                ready();
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    mMoved = false;
                    mDownX = event.getX();
                    log("touch: down");
                    device("touch: down at " + dp(event.getX()) + ", " + dp(event.getY()) + " dp");
                    break;
                case MotionEvent.ACTION_MOVE:
                    mMoved = true;
                    break;
                case MotionEvent.ACTION_UP:
                    log("touch: up, moved " + mMoved);
                    float third = getWidth() / 3f;
                    if (Math.abs(event.getX() - mDownX) < 10 * density() && mDownX > third
                            && mDownX < 2 * third) {
                        done();
                    }
                    break;
                case MotionEvent.ACTION_CANCEL:
                    // Whether a move came before the cancel is a matter of timing.
                    log("touch: cancel");
                    device("touch: cancel, moved " + mMoved);
                    break;
                default:
                    break;
            }
            return true;
        }
    }

    private Pad mPad;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        mDeviceTag = mTag + "-device";
        mPad = new Pad(this);
        setContentView(mPad);
        if (mTag.contains("-immersive")) {
            WindowInsetsController controller = getWindow().getInsetsController();
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            controller.hide(WindowInsets.Type.systemBars());
        }
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, mBack);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mReady && mPad.getWidth() > 0) {
            ready();
        }
    }

    private void ready() {
        mReady = true;
        log("ready");
        device("ready: " + dp(mPad.getWidth()) + " x " + dp(mPad.getHeight()) + " dp at density " + density());
    }

    private void done() {
        if (!mDone) {
            log("done");
            mDone = true;
        }
    }

    private float density() {
        return getResources().getDisplayMetrics().density;
    }

    private int dp(float px) {
        return Math.round(px / density());
    }

    private static String edge(int edge) {
        return edge == BackEvent.EDGE_LEFT ? "left" : edge == BackEvent.EDGE_RIGHT ? "right" : "other (" + edge + ")";
    }

    private void log(String text) {
        if (!mDone) {
            Log.i(mTag, text);
        }
    }

    private void device(String text) {
        if (!mDone) {
            Log.i(mDeviceTag, text);
        }
    }
}
