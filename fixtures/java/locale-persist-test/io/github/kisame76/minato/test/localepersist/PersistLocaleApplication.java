package io.github.kisame76.minato.test.localepersist;

import android.app.Application;
import android.app.LocaleManager;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.util.Log;

import java.io.File;

/**
 * The app's languages across starts (the compatibility plan's decision 6): the first start sets de-DE and waits for
 * the change; the second start, after the process was killed, finds de-DE stored and in its Resources already in
 * onCreate, and resets it. tools/android-reference.sh ends the process between the starts with `am kill`.
 */
public class PersistLocaleApplication extends Application {
    private static final String TAG = "locale-persist-test";

    @Override
    public void onCreate() {
        super.onCreate();
        final LocaleManager locales = getSystemService(LocaleManager.class);
        final File marker = new File(getFilesDir(), "started");
        final boolean second = marker.exists();
        Log.i(TAG, second ? "start 2" : "start 1");
        Log.i(TAG, "application locales in onCreate: " + locales.getApplicationLocales().toLanguageTags());
        Log.i(TAG, "resources locale in onCreate: " + getResources().getConfiguration().getLocales().toLanguageTags());
        if (!second) {
            try {
                marker.createNewFile();
            } catch (java.io.IOException e) {
                Log.e(TAG, "marker: " + e);
            }
            locales.setApplicationLocales(LocaleList.forLanguageTags("de-DE"));
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                Log.i(TAG, "application locales set: " + locales.getApplicationLocales().toLanguageTags());
                Log.i(TAG, "done");
            }, 2000);
        } else {
            locales.setApplicationLocales(LocaleList.getEmptyLocaleList());
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                Log.i(TAG, "application locales after the reset: " + locales.getApplicationLocales().isEmpty());
                Log.i(TAG, "done");
            }, 2000);
        }
    }
}
