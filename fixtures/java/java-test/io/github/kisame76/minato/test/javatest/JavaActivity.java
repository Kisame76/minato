package io.github.kisame76.minato.test.javatest;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * ADR-0039's test 2 (java-test), as ADR-0043 step c1 cuts it: an activity that the host's lifecycle drives. It logs
 * each lifecycle callback with what it sees, the window's focus and the decor view's size at its first layout, and
 * counts its starts in SharedPreferences, written with apply(), which the stop waits for (QueuedWork), so that a second
 * start shows the first one's write. Each start finishes itself a second after its second focus, past a background
 * and a return. The end's onDestroy logs the last line, "done". Under java-test-device, which is only shown, it logs
 * the wall clock in milliseconds at each onResume, from which ADR-0039 c6 measures the iPhone's tap to onResume on the
 * iPhone's own clock.
 */
public class JavaActivity extends Activity {
    private static final String TAG = "java-test";
    private int mStarts;
    private int mFocuses;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SharedPreferences preferences = getSharedPreferences("java-test", MODE_PRIVATE);
        mStarts = preferences.getInt("starts", 0);
        Log.i(TAG, "onCreate: saved state " + (savedInstanceState != null) + ", starts before " + mStarts
                + ", target " + getApplicationInfo().targetSdkVersion);
        preferences.edit().putInt("starts", mStarts + 1).apply();
        setContentView(new View(this));
        View decor = getWindow().getDecorView();
        decor.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                decor.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                Log.i(TAG, "layout: decor " + decor.getWidth() + " x " + decor.getHeight());
            }
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.i(TAG, "onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
        Log.i(TAG + "-device", "onResume at " + System.currentTimeMillis());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(TAG, "focus " + hasFocus);
        if (hasFocus && ++mFocuses == 2) {
            getWindow().getDecorView().postDelayed(() -> {
                Log.i(TAG, "finish");
                finish();
            }, 1000);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.i(TAG, "onPause, finishing " + isFinishing());
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "onStop, finishing " + isFinishing());
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putInt("starts", mStarts);
        Log.i(TAG, "onSaveInstanceState");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy, finishing " + isFinishing());
        Log.i(TAG, "done");
    }
}
