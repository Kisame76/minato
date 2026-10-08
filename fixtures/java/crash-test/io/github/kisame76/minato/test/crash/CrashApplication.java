package io.github.kisame76.minato.test.crash;

import android.app.Application;
import android.util.Log;

/**
 * Installs a default handler for the exceptions that no thread catches, as a crash reporter does at the start: it logs
 * the exception and its thread, then hands the exception on to the handler before it, Android's, which ends the
 * process.
 */
public class CrashApplication extends Application {
    static final String TAG = "crash-test";

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Log.i(TAG, "a default handler before ours: " + (previous != null));
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable e) {
                Log.i(TAG, "uncaught on " + thread.getName() + ": " + e);
                Log.i(TAG, "done");
                previous.uncaughtException(thread, e);
            }
        });
    }
}
