// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Activity's splash screen. (ADR-0039 step c2c3) Activity's splash screen.
class SplashScreen {

    /** @hide */
    class SplashScreenImpl implements SplashScreen {
        public SplashScreenImpl(android.content.Context context) {
            throw new RuntimeException("stub");
        }

        public void setOnExitAnimationListener(OnExitAnimationListener listener) {
            throw new RuntimeException("stub");
        }

        public void clearOnExitAnimationListener() {
            throw new RuntimeException("stub");
        }

        public void setSplashScreenTheme(int themeId) {
            throw new RuntimeException("stub");
        }
    }
}
