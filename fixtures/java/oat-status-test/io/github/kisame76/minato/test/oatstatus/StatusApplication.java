package io.github.kisame76.minato.test.oatstatus;

import android.app.Application;
import android.util.Log;
import dalvik.system.DexFile;

/**
 * What ART says about the odex next to this app's own APK (M6 c1, test 3). The line under the app's tag, its label,
 * is the answer of the public DexFile.isDexOptNeeded: false when an odex that ART accepts is next to the APK (made
 * for this dex file at this location, filter "verify" or better), true when there is none or when it was made for
 * other dex code. The boot class path does not count for a "verify" odex: ART checks its checksums only for filters
 * that depend on the boot image, and the vdex's verifier dependencies guard what verification assumed. The same on
 * Android 15 and in Minato.
 */
public class StatusApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        String tag = String.valueOf(getApplicationInfo().loadLabel(getPackageManager()));
        String apk = getApplicationInfo().sourceDir;
        try {
            Log.i(tag, "dex opt needed: " + DexFile.isDexOptNeeded(apk));
        } catch (Exception e) {
            Log.i(tag, "dex opt needed: " + e);
        }
        Log.i(tag, "done");
    }
}
