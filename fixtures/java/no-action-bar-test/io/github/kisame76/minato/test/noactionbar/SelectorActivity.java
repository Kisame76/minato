package io.github.kisame76.minato.test.noactionbar;

import android.app.Activity;
import android.view.Window;

/** The first step: a theme chain like a game engine's (NoActionBar.Fullscreen parents) and FEATURE_NO_TITLE. */
public class SelectorActivity extends StepActivity {
    @Override
    int feature() {
        return Window.FEATURE_NO_TITLE;
    }

    @Override
    Class<? extends Activity> next() {
        return BarThemeActivity.class;
    }
}
