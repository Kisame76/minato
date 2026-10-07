package io.github.kisame76.minato.test.launchmode;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/**
 * The flow (ADR-0039 step b3.5b2-2b): the activities report their lifecycle, new intents and focus here, and each step
 * waits until the steps before it have settled on Android, so that the log's order never hangs on when a window's
 * focus comes or when the system stops or destroys an activity. Main and Plain run more than once: a name's counts
 * add up over its instances, and its activity is the newest one.
 */
public class ModeApplication extends Application {
    static final String TAG = "launch-mode-test";
    static final String N = "n";
    static final int GAINS = 0;
    static final int CREATES = 1;
    static final int NEW_INTENTS = 2;
    static final int RESUMES = 3;
    static final int PAUSES = 4;
    static final int STOPS = 5;
    static final int DESTROYS = 6;

    private static ModeApplication sApplication;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    /** Per name, its newest activity. */
    private final Map<String, ModeActivity> mActivities = new HashMap<>();
    /** Per name, how often its instances were created, got a new intent, resumed, paused, stopped and went. */
    private final Map<String, int[]> mCounts = new HashMap<>();
    private int mStep;

    static ModeApplication get() {
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

    void created(ModeActivity activity) {
        mActivities.put(activity.name(), activity);
        count(activity, CREATES);
    }

    /** One of the counted events; the next step looks after the callback has returned, as an app's own post would. */
    void count(ModeActivity activity, int event) {
        counts(activity.name())[event]++;
        mHandler.post(this::advance);
    }

    private int[] counts(String name) {
        int[] counts = mCounts.get(name);
        if (counts == null) {
            counts = new int[7];
            mCounts.put(name, counts);
        }
        return counts;
    }

    private int count(String name, int event) {
        return counts(name)[event];
    }

    /** Whether the name's newest activity is between its onResume and its onPause. */
    private boolean resumed(String name) {
        ModeActivity activity = mActivities.get(name);
        return activity != null && activity.mResumedNow;
    }

    private Activity activity(String name) {
        return mActivities.get(name);
    }

    private static Intent start(Activity from, Class<?> to, String n) {
        return new Intent(from, to).putExtra(N, n);
    }

    private void advance() {
        switch (mStep) {
            case 0:
                if (count("Main", GAINS) >= 1) {
                    mStep++;
                    log("Main starts Top");
                    activity("Main").startActivity(start(activity("Main"), TopActivity.class, "first"));
                }
                break;
            case 1:
                if (count("Top", GAINS) >= 1 && count("Main", STOPS) >= 1) {
                    mStep++;
                    log("Top starts Top for result 7");
                    activity("Top").startActivityForResult(start(activity("Top"), TopActivity.class, "again"), 7);
                }
                break;
            case 2:
                if (count("Top", NEW_INTENTS) >= 1 && resumed("Top")) {
                    mStep++;
                    log("Top starts Plain");
                    activity("Top").startActivity(start(activity("Top"), PlainActivity.class, "first"));
                }
                break;
            case 3:
                if (count("Plain", GAINS) >= 1 && count("Top", STOPS) >= 1) {
                    mStep++;
                    log("Plain starts Top with CLEAR_TOP and SINGLE_TOP");
                    activity("Plain").startActivity(start(activity("Plain"), TopActivity.class, "clear top")
                            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
                }
                break;
            case 4:
                if (resumed("Top") && count("Plain", DESTROYS) >= 1) {
                    mStep++;
                    log("Top starts Plain again");
                    activity("Top").startActivity(start(activity("Top"), PlainActivity.class, "second"));
                }
                break;
            case 5:
                if (count("Plain", GAINS) >= 2 && count("Top", STOPS) >= 2) {
                    mStep++;
                    log("Plain starts Top with REORDER_TO_FRONT");
                    activity("Plain").startActivity(start(activity("Plain"), TopActivity.class, "reorder")
                            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
                }
                break;
            case 6:
                if (resumed("Top") && count("Top", NEW_INTENTS) >= 3 && count("Plain", STOPS) >= 2) {
                    mStep++;
                    log("Top starts Main with CLEAR_TOP");
                    activity("Top").startActivity(start(activity("Top"), MainActivity.class, "clear top")
                            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP));
                }
                break;
            case 7:
                if (count("Main", CREATES) >= 2 && resumed("Main") && count("Main", DESTROYS) >= 1
                        && count("Top", DESTROYS) >= 1 && count("Plain", DESTROYS) >= 2) {
                    mStep++;
                    log("Main starts Task");
                    activity("Main").startActivity(start(activity("Main"), TaskActivity.class, "first"));
                }
                break;
            case 8:
                if (count("Task", GAINS) >= 1 && count("Main", STOPS) >= 2) {
                    mStep++;
                    log("Task starts NoHist");
                    activity("Task").startActivity(start(activity("Task"), NoHistActivity.class, "first"));
                }
                break;
            case 9:
                if (count("NoHist", GAINS) >= 1 && count("Task", STOPS) >= 1) {
                    mStep++;
                    log("NoHist starts Plain");
                    activity("NoHist").startActivity(start(activity("NoHist"), PlainActivity.class, "third"));
                }
                break;
            case 10:
                if (count("Plain", GAINS) >= 3 && count("NoHist", DESTROYS) >= 1) {
                    mStep++;
                    log("Plain starts Task");
                    activity("Plain").startActivity(start(activity("Plain"), TaskActivity.class, "again"));
                }
                break;
            case 11:
                if (resumed("Task") && count("Task", NEW_INTENTS) >= 1 && count("Plain", DESTROYS) >= 3) {
                    mStep++;
                    log("Task starts Up");
                    activity("Task").startActivity(start(activity("Task"), UpActivity.class, "first"));
                }
                break;
            case 12:
                if (count("Up", GAINS) >= 1 && count("Task", STOPS) >= 2) {
                    mStep++;
                    Activity up = activity("Up");
                    Intent main = new Intent(up, MainActivity.class).putExtra(N, "up");
                    log("Up should recreate the task for Main: " + up.shouldUpRecreateTask(main));
                    log("Up navigates up to Main: " + up.navigateUpTo(main));
                }
                break;
            case 13:
                if (count("Main", CREATES) >= 3 && resumed("Main") && count("Main", DESTROYS) >= 2
                        && count("Up", DESTROYS) >= 1 && count("Task", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts Clear with NEW_TASK and CLEAR_TASK");
                    activity("Main").startActivity(start(activity("Main"), ClearActivity.class, "first")
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                }
                break;
            case 14:
                if (count("Clear", GAINS) >= 1 && count("Main", DESTROYS) >= 3) {
                    mStep++;
                    log("Clear is the task's root: " + activity("Clear").isTaskRoot());
                    log("Clear finishes");
                    activity("Clear").finish();
                }
                break;
            default:
                break;
        }
    }
}
