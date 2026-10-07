package io.github.kisame76.minato.test.gestureflow;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;

/**
 * The launcher: says it is ready at its first focus, starts Second at a gesture's DOWN and logs how its touch ends,
 * each action once; its MOVEs are not logged, as their count depends on timing.
 */
public class FirstActivity extends Activity {
    private boolean mFocused;
    private boolean mDown;
    private boolean mEnded;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View pad = new View(this);
        pad.setOnTouchListener((view, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    if (!mDown) {
                        mDown = true;
                        GestureApplication.log("First DOWN, starts Second");
                        startActivity(new Intent(this, SecondActivity.class));
                    }
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    if (mDown && !mEnded) {
                        mEnded = true;
                        GestureApplication.log("First " + MotionEvent.actionToString(event.getActionMasked()));
                        GestureApplication.firstEnded();
                    }
                    break;
                default:
                    break;
            }
            return true;
        });
        setContentView(pad);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mFocused) {
            mFocused = true;
            GestureApplication.log("First ready");
        }
    }
}
