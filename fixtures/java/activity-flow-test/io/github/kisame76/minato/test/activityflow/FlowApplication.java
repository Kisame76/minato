package io.github.kisame76.minato.test.activityflow;

import android.app.Activity;
import android.app.Application;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.AndroidRuntimeException;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/**
 * The flow (ADR-0039 step b3.5b2-1): the activities report their lifecycle and their focus here, and each step waits
 * until the steps before it have settled on Android, so that the log's order never hangs on when a window's focus
 * comes or when the system stops the activity below: a step runs once the activity it acts in has focus and the one
 * before it is stopped or destroyed.
 */
public class FlowApplication extends Application {
    static final String TAG = "activity-flow-test";
    static final int GAINS = 0;
    static final int LOSSES = 1;
    static final int STOPS = 2;
    static final int DESTROYS = 3;

    private static FlowApplication sApplication;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    /** Per name, the activity; each class starts once. */
    private final Map<String, FlowActivity> mActivities = new HashMap<>();
    /** Per name, how often it gained and lost the focus, stopped and was destroyed. */
    private final Map<String, int[]> mCounts = new HashMap<>();
    private int mStep;

    static FlowApplication get() {
        return sApplication;
    }

    static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sApplication = this;
    }

    void created(FlowActivity activity) {
        mActivities.put(activity.name(), activity);
    }

    /** One of the counted events; the next step looks after the callback has returned, as an app's own post would. */
    void count(FlowActivity activity, int event) {
        counts(activity.name())[event]++;
        mHandler.post(this::advance);
    }

    private int[] counts(String name) {
        int[] counts = mCounts.get(name);
        if (counts == null) {
            counts = new int[4];
            mCounts.put(name, counts);
        }
        return counts;
    }

    private int count(String name, int event) {
        return counts(name)[event];
    }

    private void advance() {
        switch (mStep) {
            case 0:
                if (count("Splash", GAINS) >= 1) {
                    mStep++;
                    Activity splash = mActivities.get("Splash");
                    log("Splash starts Main and finishes");
                    splash.startActivity(new Intent(splash, MainActivity.class));
                    splash.finish();
                }
                break;
            case 1:
                if (count("Main", GAINS) >= 1 && count("Splash", DESTROYS) >= 1) {
                    mStep++;
                    Activity main = mActivities.get("Main");
                    log("Main is the task's root: " + main.isTaskRoot());
                    startMissing(main);
                    startWithoutNewTask();
                    startUnhandled(main);
                    startFileUri(main);
                    log("Main starts Second");
                    main.startActivity(new Intent(main, SecondActivity.class));
                }
                break;
            case 2:
                if (count("Second", GAINS) >= 1 && count("Main", STOPS) >= 1) {
                    mStep++;
                    log("Second finishes");
                    mActivities.get("Second").finish();
                }
                break;
            case 3:
                if (count("Main", GAINS) >= 2 && count("Second", DESTROYS) >= 1) {
                    mStep++;
                    log("The Application starts Third with FLAG_ACTIVITY_NEW_TASK");
                    startActivity(new Intent(this, ThirdActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                }
                break;
            case 4:
                if (count("Third", GAINS) >= 1 && count("Main", STOPS) >= 2) {
                    mStep++;
                    Activity third = mActivities.get("Third");
                    log("Third is the task's root: " + third.isTaskRoot() + ", in Main's task: "
                            + (third.getTaskId() == mActivities.get("Main").getTaskId()));
                    log("Third finishes");
                    third.finish();
                }
                break;
            case 5:
                if (count("Main", GAINS) >= 3 && count("Third", DESTROYS) >= 1) {
                    mStep++;
                    log("focus gained: Splash " + count("Splash", GAINS) + ", Main " + count("Main", GAINS)
                            + ", Second " + count("Second", GAINS) + ", Third " + count("Third", GAINS)
                            + "; Main lost it " + count("Main", LOSSES) + " times");
                    log("done");
                }
                break;
            default:
                break;
        }
    }

    /** An explicit start of a class that the manifest lacks. */
    private static void startMissing(Activity activity) {
        String packageName = activity.getPackageName();
        try {
            activity.startActivity(new Intent().setClassName(packageName, packageName + ".MissingActivity"));
            log("MissingActivity started");
        } catch (ActivityNotFoundException e) {
            log("MissingActivity: " + e);
        }
    }

    /** An implicit intent that no activity handles. */
    private static void startUnhandled(Activity activity) {
        try {
            activity.startActivity(new Intent(activity.getPackageName() + ".NOTHING"));
            log("NOTHING started");
        } catch (ActivityNotFoundException e) {
            log("NOTHING: " + e);
        }
    }

    /** A file URI that leaves the app, which StrictMode's defaults refuse from target 24 on. */
    private static void startFileUri(Activity activity) {
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("file:///sdcard/flow.txt")));
            log("file URI started");
        } catch (RuntimeException e) {
            log("file URI: " + e);
        }
    }

    /** A start from the Application without FLAG_ACTIVITY_NEW_TASK, which Android refuses from target 28 (P) on. */
    private void startWithoutNewTask() {
        try {
            startActivity(new Intent(this, SecondActivity.class));
            log("Second started without FLAG_ACTIVITY_NEW_TASK");
        } catch (AndroidRuntimeException e) {
            log("Without FLAG_ACTIVITY_NEW_TASK: " + e);
        }
    }
}
