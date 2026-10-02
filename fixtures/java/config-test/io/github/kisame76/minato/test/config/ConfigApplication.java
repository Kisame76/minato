package io.github.kisame76.minato.test.config;

import android.app.Application;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.util.Log;

/**
 * ADR-0039's test 3: the system Resources' DisplayMetrics and Configuration, logged from the Application class's
 * static initializer. On Android, handleBindApplication has given them the process's configuration and the
 * display's metrics by then (ResourcesManager.applyConfigurationToResources), so they are the display's. The lines
 * with the tag config-test must equal the AVD's on the M4 plan's display (1290 x 2796 px at density 480); those
 * with config-test-device differ by device and are only shown.
 */
public class ConfigApplication extends Application {
    private static final String TAG = "config-test";
    private static final String DEVICE_TAG = "config-test-device";

    static {
        try {
            report();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    @SuppressWarnings("deprecation") // scaledDensity, which apps still read.
    static void report() {
        DisplayMetrics metrics = Resources.getSystem().getDisplayMetrics();
        Log.i(TAG, "metrics widthPixels " + metrics.widthPixels + ", heightPixels " + metrics.heightPixels);
        Log.i(TAG, "metrics density " + metrics.density + ", densityDpi " + metrics.densityDpi + ", scaledDensity "
                + metrics.scaledDensity);
        Log.i(TAG, "metrics DENSITY_DEVICE_STABLE " + DisplayMetrics.DENSITY_DEVICE_STABLE);
        Log.i(DEVICE_TAG, "metrics xdpi " + metrics.xdpi + ", ydpi " + metrics.ydpi);

        Configuration config = Resources.getSystem().getConfiguration();
        Log.i(TAG, "config screenWidthDp " + config.screenWidthDp + ", screenHeightDp " + config.screenHeightDp
                + ", smallestScreenWidthDp " + config.smallestScreenWidthDp);
        Log.i(TAG, "config densityDpi " + config.densityDpi + ", fontScale " + config.fontScale
                + ", fontWeightAdjustment " + config.fontWeightAdjustment);
        Log.i(TAG, "config orientation " + config.orientation + ", screenLayout 0x"
                + Integer.toHexString(config.screenLayout) + ", uiMode 0x" + Integer.toHexString(config.uiMode));
        Log.i(TAG, "config touchscreen " + config.touchscreen + ", keyboard " + config.keyboard + ", keyboardHidden "
                + config.keyboardHidden + ", hardKeyboardHidden " + config.hardKeyboardHidden);
        Log.i(TAG, "config navigation " + config.navigation + ", navigationHidden " + config.navigationHidden);
        Log.i(TAG, "config locales " + config.getLocales().toLanguageTags() + ", layoutDirection "
                + config.getLayoutDirection());
        Log.i(DEVICE_TAG, "config mcc " + config.mcc + ", mnc " + config.mnc + ", colorMode 0x"
                + Integer.toHexString(config.colorMode));
    }
}
