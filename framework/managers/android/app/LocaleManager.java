// The written part of LocaleManager (retest 12: Helpshift asks it for the app's language), which tools/managergen joins
// with the API's methods. AOSP's class asks the system's locale manager service; ours keeps the app's languages in the
// process: setApplicationLocales stores them and hands the main thread a configuration change, in which the app's
// languages come before the system's, as it reaches the Application and the activities on Android [verified:
// locale-test on apk-runner-35]. An app that has set none gets an empty list, as Android answers for an app whose user
// picked none, and the system's languages are the system configuration's. The choice is kept in the app's data
// folder and is in the app's Resources when it starts again, as Android stores it per app [verified:
// locale-persist-test on apk-runner-35].
import android.content.Context;
import android.content.res.Resources;
import android.os.LocaleList;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

class LocaleManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry makes for "locale" (its service is ours: none). */
    public LocaleManager(Context context) {
        mContext = context;
    }

    private static LocaleList sApplicationLocales = LocaleList.getEmptyLocaleList();
    /** The system's languages, kept when the app first sets its own, as the system configuration then holds both. */
    private static LocaleList sSystemLocales;

    public LocaleList getApplicationLocales() {
        synchronized (LocaleManager.class) {
            return sApplicationLocales;
        }
    }

    public LocaleList getSystemLocales() {
        synchronized (LocaleManager.class) {
            return sSystemLocales != null ? sSystemLocales : Resources.getSystem().getConfiguration().getLocales();
        }
    }

    public void setApplicationLocales(LocaleList locales) {
        final LocaleList app = locales == null ? LocaleList.getEmptyLocaleList() : locales;
        final LocaleList system;
        synchronized (LocaleManager.class) {
            if (sSystemLocales == null) {
                sSystemLocales = Resources.getSystem().getConfiguration().getLocales();
            }
            sApplicationLocales = app;
            system = sSystemLocales;
        }
        store(app);
        final ActivityThread thread = ActivityThread.currentActivityThread();
        if (thread != null) {
            thread.applyAppLocales(app, system);
        }
    }

    private static File storedFile() {
        final Application application = ActivityThread.currentApplication();
        return application == null ? null : new File(application.getDataDir(), "app-locales");
    }

    /** Keeps the app's languages in its data folder, as language tags, or forgets them when there are none. */
    private static void store(LocaleList app) {
        final File file = storedFile();
        if (file == null) {
            return;
        }
        try {
            if (app.isEmpty()) {
                Files.deleteIfExists(file.toPath());
            } else {
                Files.write(file.toPath(), app.toLanguageTags().getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException e) {
            Log.w("LocaleManager", "The app's languages could not be kept: " + e);
        }
    }

    /** @hide The app's start: its stored languages are in its Resources before any component runs. */
    static void restoreAtStart(ActivityThread thread, Application application) {
        final File file = new File(application.getDataDir(), "app-locales");
        if (!file.isFile()) {
            return;
        }
        try {
            final LocaleList app = LocaleList.forLanguageTags(
                    new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
            if (app.isEmpty()) {
                return;
            }
            final LocaleList system = Resources.getSystem().getConfiguration().getLocales();
            synchronized (LocaleManager.class) {
                sSystemLocales = system;
                sApplicationLocales = app;
            }
            thread.applyAppLocalesAtStart(app, system);
        } catch (IOException | RuntimeException e) {
            Log.w("LocaleManager", "The app's stored languages could not be read: " + e);
        }
    }
}
