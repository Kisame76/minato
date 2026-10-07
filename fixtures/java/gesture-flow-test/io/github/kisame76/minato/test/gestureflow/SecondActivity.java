package io.github.kisame76.minato.test.gestureflow;

import android.app.Activity;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;

/**
 * Started at First's DOWN: logs the action of the first touch event its window gets, before any view could ignore one
 * that lacks its DOWN, then its UP and "done".
 */
public class SecondActivity extends Activity {
    private boolean mFocused;
    private boolean mTouched;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(new View(this));
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (!mTouched) {
            mTouched = true;
            GestureApplication.log("Second's first event " + MotionEvent.actionToString(action));
        }
        if (action == MotionEvent.ACTION_UP) {
            GestureApplication.log("Second UP");
            GestureApplication.log("done");
        }
        return super.dispatchTouchEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mFocused) {
            mFocused = true;
            GestureApplication.secondFocused();
        }
    }
}
