package io.github.kisame76.minato.test.devprofile;

import android.app.Application;
import android.os.Build;
import android.util.Log;

/**
 * The names android.os.Build gives an app about its device (ADR-0065). What does not depend on the device is logged
 * under the app's own tag, its label, and is the same on Android 15 and in Minato with any device profile: every name
 * but the manufacturer (the emulator's is "unknown") is set, and the fingerprint is made of the brand, product,
 * device and release. The values themselves (the emulator's, which is a userdebug build with test keys, or a
 * profile's, with Minato's user build and release keys) go under the tag with -device, where Minato's check looks
 * for the profile's.
 */
public class ProfileApplication extends Application {
    private String mTag;

    private void log(String line) {
        Log.i(mTag, line);
    }

    private void device(String line) {
        Log.i(mTag + "-device", line);
    }

    private static boolean set(String value) {
        return value != null && !value.isEmpty() && !value.equals(Build.UNKNOWN);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mTag = String.valueOf(getApplicationInfo().loadLabel(getPackageManager()));
        log("brand set: " + set(Build.BRAND));
        log("model set: " + set(Build.MODEL));
        log("device set: " + set(Build.DEVICE));
        log("product set: " + set(Build.PRODUCT));
        log("hardware set: " + set(Build.HARDWARE));
        log("fingerprint made of the names: " + Build.FINGERPRINT.startsWith(
                Build.BRAND + "/" + Build.PRODUCT + "/" + Build.DEVICE + ":" + Build.VERSION.RELEASE + "/"));
        device("manufacturer: " + Build.MANUFACTURER);
        device("brand: " + Build.BRAND);
        device("model: " + Build.MODEL);
        device("device: " + Build.DEVICE);
        device("product: " + Build.PRODUCT);
        device("hardware: " + Build.HARDWARE);
        device("soc manufacturer: " + Build.SOC_MANUFACTURER);
        device("soc model: " + Build.SOC_MODEL);
        device("type: " + Build.TYPE + " tags: " + Build.TAGS);
        device("fingerprint: " + Build.FINGERPRINT);
        log("done");
    }
}
