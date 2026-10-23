package io.github.kisame76.minato.test.title;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/**
 * title-test's activity on Theme.Material.Light, whose title is in its action bar: the window has no title bar of its
 * own (no android.R.id.title), then its finish. Where its content starts depends on the action bar, which Minato has
 * not yet, so that goes to the -device tag.
 */
public class ActionBarTitleActivity extends Activity {
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("An activity with an action bar");
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
            TitleActivity.describe("action bar activity", getWindow(), true);
            finish();
            main.next(8);
        }, TitleActivity.SETTLE_MS);
    }
}
