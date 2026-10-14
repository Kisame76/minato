package io.github.kisame76.minato.test.components;

import android.service.media.CameraPrewarmService;

/** A camera prewarm service, which an app declares and only the system binds. */
public class PrewarmService extends CameraPrewarmService {
    @Override
    public void onPrewarm() {
    }

    @Override
    public void onCooldown(boolean fired) {
    }
}
