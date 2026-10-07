package io.github.kisame76.minato.test.activityflow;

import android.graphics.PixelFormat;
import android.os.Bundle;

/**
 * Started by Main; finishes once Main has stopped. Its window's format has alpha, so a host shows it on a layer that is
 * not opaque, while Main's is; Third's has alpha too.
 */
public class SecondActivity extends FlowActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setFormat(PixelFormat.RGBA_8888);
    }
}
