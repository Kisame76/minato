// The written part of LocaleManager (retest 12: Helpshift asks it for the app's language), which tools/managergen joins
// with the API's methods. AOSP's class asks the system's locale manager service; ours keeps the app's languages in the
// process: setApplicationLocales stores them and hands the main thread a configuration change, in which the app's
// languages come before the system's, as it reaches the Application and the activities on Android [verified:
// locale-test on apk-runner-35]. An app that has set none gets an empty list, as Android answers for an app whose user
// picked none, and the system's languages are the system configuration's. A named difference: the choice does not
// outlive the process (Android stores it per app; a Settings idea: per-app language).
import android.content.Context;
import android.content.res.Resources;
import android.os.LocaleList;

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
        final ActivityThread thread = ActivityThread.currentActivityThread();
        if (thread != null) {
            thread.applyAppLocales(app, system);
        }
    }
}
