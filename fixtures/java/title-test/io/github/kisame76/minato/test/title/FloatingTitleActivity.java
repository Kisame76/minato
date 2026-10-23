package io.github.kisame76.minato.test.title;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/** title-test's dialog-themed activity: its label as its title, then setTitle, then its finish. */
public class FloatingTitleActivity extends Activity {
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("A dialog-themed activity");
        text.setPadding(24, 24, 24, 24);
        setContentView(text);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        TitleActivity main = TitleActivity.sInstance;
        main.mHandler.postDelayed(() -> {
            TitleActivity.describe("floating activity", getWindow(), false);
            setTitle("Floating changed");
            main.mHandler.postDelayed(() -> {
                TitleActivity.describe("floating activity after setTitle", getWindow(), false);
                finish();
                main.next(6);
            }, TitleActivity.SETTLE_MS);
        }, TitleActivity.SETTLE_MS);
    }
}
