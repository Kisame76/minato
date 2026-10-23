package io.github.kisame76.minato.test.title;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/** title-test's activity on Theme.Light: a title bar without an action bar, its label as its title, then its finish. */
public class ClassicTitleActivity extends Activity {
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("A classic activity");
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
            TitleActivity.describe("classic activity", getWindow(), true);
            finish();
            main.next(7);
        }, TitleActivity.SETTLE_MS);
    }
}
