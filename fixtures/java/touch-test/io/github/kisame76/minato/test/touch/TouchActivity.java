package io.github.kisame76.minato.test.touch;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;

/**
 * ADR-0045 step 7b2's test of the input channel: a view that fills the window, which logs what reaches it of a tap in
 * the window's middle and a swipe to the right through it, as the host plays them: apk-run's tap and swipe steps on the
 * Mac, adb shell input on the AVD. The lines name the events' actions, pointer counts, source and tool, the click, the
 * GestureDetector's taps and the fling's direction, whether MOVEs came before the end and whether one had history,
 * then "done" at the swipe's click. Places, pressures, the device, the counts of MOVEs and samples and the velocities
 * depend on how the host samples, so they go under the device tag.
 */
public class TouchActivity extends Activity {
    private static final String TAG = "touch-test";
    private static final String DEVICE_TAG = "touch-test-device";

    private boolean mReady;
    private int mClicks;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Pad pad = new Pad(this);
        pad.setOnClickListener(view -> {
            Log.i(TAG, "pad: click");
            if (++mClicks == 2) {
                Log.i(TAG, "done");
            }
        });
        setContentView(pad);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mReady) {
            mReady = true;
            View pad = findViewById(android.R.id.content);
            Log.i(TAG, "pad: ready");
            Log.i(DEVICE_TAG, "pad: content " + pad.getWidth() + " x " + pad.getHeight());
        }
    }

    /** The view: each event logged, then GestureDetector's, then View's own handling, which clicks. */
    private static final class Pad extends View {
        private final GestureDetector mDetector;
        private int mMoves;
        private int mSamples;
        private int mWithHistory;
        private boolean mScrolled;

        Pad(Context context) {
            super(context);
            mDetector = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
                @Override
                public boolean onDown(MotionEvent e) {
                    Log.i(TAG, "gesture: down");
                    return true;
                }

                @Override
                public boolean onSingleTapUp(MotionEvent e) {
                    Log.i(TAG, "gesture: single tap up");
                    return true;
                }

                @Override
                public boolean onSingleTapConfirmed(MotionEvent e) {
                    Log.i(TAG, "gesture: single tap confirmed");
                    return true;
                }

                @Override
                public boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY) {
                    if (!mScrolled) {
                        mScrolled = true;
                        Log.i(TAG, "gesture: scroll");
                    }
                    return true;
                }

                @Override
                public boolean onFling(MotionEvent e1, MotionEvent e2, float velocityX, float velocityY) {
                    String direction = Math.abs(velocityX) >= Math.abs(velocityY)
                            ? (velocityX > 0 ? "right" : "left") : (velocityY > 0 ? "down" : "up");
                    Log.i(TAG, "gesture: fling " + direction);
                    Log.i(DEVICE_TAG, "gesture: fling " + velocityX + " " + velocityY + " px/s");
                    return true;
                }
            });
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_MOVE) {
                mMoves++;
                mSamples += event.getHistorySize() + 1;
                if (event.getHistorySize() > 0) {
                    mWithHistory++;
                }
            } else {
                if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                    Log.i(TAG, "pad: moved before the end: " + (mMoves > 0) + ", with history: " + (mWithHistory > 0));
                    Log.i(DEVICE_TAG, "pad: up after " + mMoves + " MOVEs with " + mSamples + " samples, "
                            + mWithHistory + " with history");
                }
                Log.i(TAG, "pad: " + MotionEvent.actionToString(event.getAction()) + ", pointers "
                        + event.getPointerCount() + ", source 0x" + Integer.toHexString(event.getSource()) + ", tool "
                        + event.getToolType(0));
                Log.i(DEVICE_TAG, "pad: " + MotionEvent.actionToString(event.getAction()) + " at " + event.getX()
                        + " " + event.getY() + ", raw " + event.getRawX() + " " + event.getRawY() + ", device "
                        + event.getDeviceId() + ", pressure " + event.getPressure() + ", "
                        + (event.getEventTime() - event.getDownTime()) + " ms after the down");
                if (action == MotionEvent.ACTION_DOWN) {
                    mMoves = 0;
                    mSamples = 0;
                    mWithHistory = 0;
                    mScrolled = false;
                }
            }
            mDetector.onTouchEvent(event);
            return super.onTouchEvent(event);
        }
    }
}
