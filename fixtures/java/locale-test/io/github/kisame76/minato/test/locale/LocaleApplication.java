package io.github.kisame76.minato.test.locale;

import android.app.Application;
import android.app.LocaleManager;
import android.content.res.Configuration;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.util.Log;

import java.util.concurrent.Callable;

/**
 * LocaleManager.setApplicationLocales (the compatibility plan's decision 6): what the app reads back after it sets
 * its languages, at once and a moment later, and whether its Application hears the configuration change.
 */
public class LocaleApplication extends Application {
    private static final String TAG = "locale-test";
    private String mChanged = "none";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            result = String.valueOf(call.call());
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        mChanged = newConfig.getLocales().toLanguageTags();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private String resourcesLocale() {
        return getResources().getConfiguration().getLocales().toLanguageTags();
    }

    private void run() {
        final LocaleManager locales = getSystemService(LocaleManager.class);
        log("application locales at the start", () -> locales.getApplicationLocales().toLanguageTags().isEmpty());
        final String system = locales.getSystemLocales().toLanguageTags();
        log("set de-DE", () -> {
            locales.setApplicationLocales(LocaleList.forLanguageTags("de-DE"));
            return "returned";
        });
        log("application locales at once", () -> locales.getApplicationLocales().toLanguageTags());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            log("application locales later", () -> locales.getApplicationLocales().toLanguageTags());
            log("resources locale later", this::resourcesLocale);
            log("Application heard the change", () -> mChanged);
            log("system locales unchanged", () -> locales.getSystemLocales().toLanguageTags().equals(system));
            locales.setApplicationLocales(LocaleList.getEmptyLocaleList());
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                log("application locales after the reset", () -> locales.getApplicationLocales().isEmpty());
                log("resources locale is the system's again", () -> resourcesLocale().equals(system));
                Log.i(TAG, "done");
            }, 2000);
        }, 2000);
    }
}
