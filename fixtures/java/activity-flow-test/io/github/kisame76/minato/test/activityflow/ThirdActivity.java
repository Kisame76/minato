package io.github.kisame76.minato.test.activityflow;

import android.graphics.PixelFormat;
import android.os.Bundle;

/**
 * Started by the Application with FLAG_ACTIVITY_NEW_TASK; finishes once Main has stopped. Its window's format has
 * alpha, as Second's has.
 */
public class ThirdActivity extends FlowActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setFormat(PixelFormat.RGBA_8888);
    }
}
