package io.github.kisame76.minato.test.pad;

import android.app.Application;
import android.content.Context;
import android.util.Log;
import com.google.android.play.core.splitcompat.SplitCompat;

/**
 * ADR-0050 c3's probe: an app that installs SplitCompat as feature-delivery asks of an app with feature modules, in
 * attachBaseContext. The library patches the app's class loader and resources by reflection so that a module
 * installed later is usable without a restart. Logs with the tag pad-test what the install gives.
 */
public class PadApplication extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            Log.i(PadActivity.TAG, "splitcompat install: " + SplitCompat.install(this));
        } catch (Throwable t) {
            Log.i(PadActivity.TAG, "splitcompat install failed: " + t.getClass().getName());
            Log.i(PadActivity.TAG + "-device", Log.getStackTraceString(t));
        }
    }
}
