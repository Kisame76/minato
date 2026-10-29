package io.github.kisame76.minato.test.imeinsets;

import android.app.Activity;

/** ResizeActivity's test with adjustPan. */
public class PanActivity extends ResizeActivity {
    @Override
    Class<? extends Activity> successor() {
        return UnspecifiedActivity.class;
    }

    @Override
    String mode() {
        return "pan";
    }
}
