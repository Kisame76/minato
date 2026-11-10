// The written part of SplashScreen (the Unity test APK retest 10: Activity's methods name the type, and a class whose method types
// cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's interface. Minato's
// host draws the starting window (ADR-0076), so the app gets no splash view and no exit animation is ever called, as
// on Android for an activity whose splash was not handed to it. The splash theme an activity sets is kept for the
// app's later starts, as Android's package manager keeps it, by its resource name.
class SplashScreen {
    /** @hide */
    class SplashScreenImpl implements SplashScreen {
        private final android.content.Context mContext;

        public SplashScreenImpl(android.content.Context context) {
            mContext = context;
        }

        public void setOnExitAnimationListener(OnExitAnimationListener listener) {
        }

        public void clearOnExitAnimationListener() {
        }

        public void setSplashScreenTheme(int themeId) {
            if (!(mContext instanceof android.app.Activity)) {
                android.util.Log.w("SplashScreen",
                        "Couldn't persist the starting theme. This instance is not an Activity");
                return;
            }
            com.android.internal.minato.StartingWindowColor.keepTheme(mContext, themeId);
        }
    }
}
