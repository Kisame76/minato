package io.github.kisame76.minato.test.activitystart;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;

/**
 * ADR-0039 step c2c3's test: an app's own Activity subclass starts as the launcher, behind an activity-alias, and its
 * window shows its content view. The Application logs its own steps and its activities' lifecycle callbacks, so that
 * the log shows where the start falls. It logs with the tag activity-start-test and ends with "done" once the window
 * has focus; tools/android-reference.sh compares it with Android 15's log.
 */
public class StartApplication extends Application implements Application.ActivityLifecycleCallbacks {
    static final String TAG = "activity-start-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        Log.i(TAG, "application attachBaseContext");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        registerActivityLifecycleCallbacks(this);
        Log.i(TAG, "application onCreate");
    }

    private static void callback(String event, Activity activity) {
        Log.i(TAG, "callback " + event + " " + activity.getClass().getSimpleName());
    }

    @Override
    public void onActivityPreCreated(Activity activity, Bundle savedInstanceState) {
        callback("preCreated", activity);
    }

    @Override
    public void onActivityCreated(Activity activity, Bundle savedInstanceState) {
        callback("created", activity);
    }

    @Override
    public void onActivityPostCreated(Activity activity, Bundle savedInstanceState) {
        callback("postCreated", activity);
    }

    @Override
    public void onActivityStarted(Activity activity) {
        callback("started", activity);
    }

    @Override
    public void onActivityResumed(Activity activity) {
        callback("resumed", activity);
    }

    @Override
    public void onActivityPostResumed(Activity activity) {
        callback("postResumed", activity);
    }

    @Override
    public void onActivityPaused(Activity activity) {
        callback("paused", activity);
    }

    @Override
    public void onActivityStopped(Activity activity) {
        callback("stopped", activity);
    }

    @Override
    public void onActivitySaveInstanceState(Activity activity, Bundle outState) {
        callback("saveInstanceState", activity);
    }

    @Override
    public void onActivityDestroyed(Activity activity) {
        callback("destroyed", activity);
    }
}
