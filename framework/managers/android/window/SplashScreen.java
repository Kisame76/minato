// The written part of SplashScreen (the Unity test APK retest 10: Activity's methods name the type, and a class whose method types
// cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's interface. Minato shows
// no system splash screen before an activity's first frame, so Activity's implementation keeps nothing: no exit
// animation is ever called, as on Android for an activity that was launched without one.
class SplashScreen {
    /** @hide */
    class SplashScreenImpl implements SplashScreen {
        public SplashScreenImpl(android.content.Context context) {
        }

        public void setOnExitAnimationListener(OnExitAnimationListener listener) {
        }

        public void clearOnExitAnimationListener() {
        }

        public void setSplashScreenTheme(int themeId) {
        }
    }
}
