package io.github.kisame76.minato.test.noactionbar;

import android.app.Activity;
import android.view.Window;

/** The second step: FEATURE_NO_TITLE on a theme with an action bar, whose request for the bar Android ignores. */
public class BarThemeActivity extends StepActivity {
    @Override
    int feature() {
        return Window.FEATURE_NO_TITLE;
    }

    @Override
    Class<? extends Activity> next() {
        return RequestedBarActivity.class;
    }
}
