package io.github.kisame76.minato.test.secondwindows;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

/**
 * The b3 part's floating activity (ADR-0041 (b3)), on Theme.Material.Light.Dialog.NoActionBar (our PhoneWindow
 * has no title bar yet, a listed gap) above SecondWindowsActivity, which stays paused below it: its window's frame,
 * which WindowLayout gives it as a dialog's, and its dim, then its finish.
 */
public class FloatingActivity extends Activity {
    private static final int SETTLE_MS = 600;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("Floating");
        text.setPadding(24, 24, 24, 24);
        setContentView(text);
        SecondWindowsActivity.sInstance.watchFocus("floating", text);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.postDelayed(this::frame, SETTLE_MS);
        }
    }

    private void frame() {
        SecondWindowsActivity main = SecondWindowsActivity.sInstance;
        main.logFocus();
        View decor = getWindow().getDecorView();
        int[] at = new int[2];
        decor.getLocationOnScreen(at);
        int screenWidth = main.getWindow().getDecorView().getWidth();
        WindowManager.LayoutParams params = getWindow().getAttributes();
        SecondWindowsActivity.log("floating decor " + decor.getWidth() + "x" + decor.getHeight() + ", left " + at[0]
                + ", centered " + (at[0] == (screenWidth - decor.getWidth()) / 2) + ", type " + params.type
                + ", dim behind " + ((params.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0) + " "
                + params.dimAmount);
        SecondWindowsActivity.logDevice("floating decor top " + at[1]);
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        SecondWindowsActivity.log("floating destroyed");
        SecondWindowsActivity.sInstance.afterFloating();
    }
}
