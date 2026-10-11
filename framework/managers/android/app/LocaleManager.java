// The written part of LocaleManager (retest 12: Helpshift asks it for the app's language), which tools/managergen joins
// with the API's methods. AOSP's class asks the system's locale manager service; Minato has no per-app languages yet,
// so the app's locales are empty, as Android answers for an app whose user picked none, and the system's are the
// system configuration's. setApplicationLocales, which on Android stores the choice and recreates the activities, is
// still unwritten (a Settings idea: per-app language).
import android.content.Context;
import android.content.res.Resources;
import android.os.LocaleList;

class LocaleManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry makes for "locale" (its service is ours: none). */
    public LocaleManager(Context context) {
        mContext = context;
    }

    public LocaleList getApplicationLocales() {
        return LocaleList.getEmptyLocaleList();
    }

    public LocaleList getSystemLocales() {
        return Resources.getSystem().getConfiguration().getLocales();
    }
}
