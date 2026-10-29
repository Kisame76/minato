package io.github.kisame76.minato.test.imeinsets;

import android.app.Activity;

/** ResizeActivity's test with adjustUnspecified, where Android picks resize or pan; the last of the run. */
public class UnspecifiedActivity extends ResizeActivity {
    @Override
    Class<? extends Activity> successor() {
        return null;
    }

    @Override
    String mode() {
        return "unspecified";
    }
}
