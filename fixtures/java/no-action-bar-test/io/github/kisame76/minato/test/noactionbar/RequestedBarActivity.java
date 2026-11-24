package io.github.kisame76.minato.test.noactionbar;

import android.app.Activity;
import android.view.Window;

/** The last step: FEATURE_ACTION_BAR on a theme without a title, whose FEATURE_NO_TITLE removes the bar on Android. */
public class RequestedBarActivity extends StepActivity {
    @Override
    int feature() {
        return Window.FEATURE_ACTION_BAR;
    }

    @Override
    Class<? extends Activity> next() {
        return null;
    }
}
