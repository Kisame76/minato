package io.github.kisame76.minato.test.startingwindow;

import android.os.Bundle;
import android.util.Log;
import android.window.SplashScreen;

/**
 * The launcher activity, whose theme sets windowSplashScreenBackground; it also makes the calls that androidx
 * core-splashscreen makes on API 31 and later, each of which must return without an exception.
 */
public class SplashColorActivity extends SlowActivity {
    @Override
    protected void onCreate(Bundle state) {
        SplashScreen splash = getSplashScreen();
        Log.i(TAG, "getSplashScreen: " + (splash != null));
        call("setOnExitAnimationListener", () -> splash.setOnExitAnimationListener(view -> view.remove()));
        call("clearOnExitAnimationListener", splash::clearOnExitAnimationListener);
        int theme = getResources().getIdentifier("SplashColor", "style", getPackageName());
        call("setSplashScreenTheme", () -> splash.setSplashScreenTheme(theme));
        super.onCreate(state);
    }

    private static void call(String name, Runnable body) {
        try {
            body.run();
            Log.i(TAG, name + ": returned");
        } catch (RuntimeException e) {
            Log.i(TAG, name + ": " + e.getClass().getName());
        }
    }
}
