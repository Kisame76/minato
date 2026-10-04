package io.github.kisame76.minato.test.appcalls;

import android.app.Application;
import android.util.Log;

/**
 * ADR-0039 step c2d's test: calls an app makes into the platform without a window. Step c2d1: Log.wtf, which logs
 * at ERROR and hands the failure to the system, which keeps an app running. The throwables have no stack, so that
 * the log is the same wherever the app runs. It logs with the tag app-calls-test and ends with "done";
 * tools/android-reference.sh compares it with Android 15's log.
 */
public class AppCallsApplication extends Application {
    static final String TAG = "app-calls-test";

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            terribleFailures();
        } catch (Throwable t) {
            Log.e(TAG, "failed", t);
        }
        Log.i(TAG, "done");
    }

    private static void terribleFailures() {
        Log.i(TAG, "Log.wtf with a message");
        report(Log.wtf(TAG, "a terrible failure"));
        Throwable cause = new IllegalStateException("its cause");
        cause.setStackTrace(new StackTraceElement[0]);
        Log.i(TAG, "Log.wtf with a message and a throwable");
        report(Log.wtf(TAG, "a terrible failure with a cause", cause));
        Log.i(TAG, "Log.wtf with a throwable");
        report(Log.wtf(TAG, cause));
    }

    private static void report(int written) {
        Log.i(TAG, "returned " + (written > 0 ? "a positive count" : String.valueOf(written)) + ", still running");
    }
}
