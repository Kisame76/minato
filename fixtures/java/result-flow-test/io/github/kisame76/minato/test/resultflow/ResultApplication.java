package io.github.kisame76.minato.test.resultflow;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.AndroidRuntimeException;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/**
 * The flow (ADR-0039 step b3.5b2-2a): the activities report their lifecycle and their focus here, and each step waits
 * until the steps before it have settled on Android, so that the log's order never hangs on when a window's focus
 * comes or when the system stops or destroys an activity: a step runs once the activity it acts in has focus or is
 * resumed, and the one before it is stopped or destroyed.
 */
public class ResultApplication extends Application {
    static final String TAG = "result-flow-test";
    static final int GAINS = 0;
    static final int RESUMES = 1;
    static final int PAUSES = 2;
    static final int STOPS = 3;
    static final int DESTROYS = 4;
    static final String FROM = "from";

    private static ResultApplication sApplication;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    /** Per name, the activity; each class starts once. */
    private final Map<String, ResultActivity> mActivities = new HashMap<>();
    /** Per name, how often it gained the focus, resumed, paused, stopped and was destroyed. */
    private final Map<String, int[]> mCounts = new HashMap<>();
    private int mStep;

    static ResultApplication get() {
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

    void created(ResultActivity activity) {
        mActivities.put(activity.name(), activity);
    }

    /** One of the counted events; the next step looks after the callback has returned, as an app's own post would. */
    void count(ResultActivity activity, int event) {
        counts(activity.name())[event]++;
        mHandler.post(this::advance);
    }

    private int[] counts(String name) {
        int[] counts = mCounts.get(name);
        if (counts == null) {
            counts = new int[5];
            mCounts.put(name, counts);
        }
        return counts;
    }

    private int count(String name, int event) {
        return counts(name)[event];
    }

    /** Whether the activity is between its onResume and its onPause. */
    private boolean resumed(String name) {
        return count(name, RESUMES) > count(name, PAUSES);
    }

    private Activity activity(String name) {
        return mActivities.get(name);
    }

    private static Intent result(String from) {
        return new Intent().putExtra(FROM, from);
    }

    private void advance() {
        switch (mStep) {
            case 0:
                if (count("Main", GAINS) >= 1) {
                    mStep++;
                    log("Main starts Ok for result 1");
                    activity("Main").startActivityForResult(new Intent(activity("Main"), OkActivity.class), 1);
                }
                break;
            case 1:
                if (count("Ok", GAINS) >= 1 && count("Main", STOPS) >= 1) {
                    mStep++;
                    log("Ok returns RESULT_OK and finishes");
                    activity("Ok").setResult(Activity.RESULT_OK, result("Ok"));
                    activity("Ok").finish();
                }
                break;
            case 2:
                if (resumed("Main") && count("Ok", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts Early for result 2");
                    activity("Main").startActivityForResult(new Intent(activity("Main"), EarlyActivity.class), 2);
                }
                break;
            case 3:
                if (resumed("Main") && count("Early", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts NewTask for result 3 with FLAG_ACTIVITY_NEW_TASK");
                    activity("Main").startActivityForResult(new Intent(activity("Main"), NewTaskActivity.class)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), 3);
                }
                break;
            case 4:
                if (count("NewTask", GAINS) >= 1 && count("Main", STOPS) >= 2) {
                    mStep++;
                    log("NewTask starts Sub for result 4");
                    activity("NewTask").startActivityForResult(new Intent(activity("NewTask"), SubActivity.class), 4);
                }
                break;
            case 5:
                if (count("Sub", GAINS) >= 1 && count("NewTask", STOPS) >= 1) {
                    mStep++;
                    log("NewTask finishes Sub with finishActivity(4)");
                    activity("NewTask").finishActivity(4);
                }
                break;
            case 6:
                if (resumed("NewTask") && count("Sub", DESTROYS) >= 1) {
                    mStep++;
                    log("NewTask finishes");
                    activity("NewTask").finish();
                }
                break;
            case 7:
                if (resumed("Main") && count("NewTask", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts Forward for result 5");
                    activity("Main").startActivityForResult(new Intent(activity("Main"), ForwardActivity.class), 5);
                }
                break;
            case 8:
                if (count("Forward", GAINS) >= 1 && count("Main", STOPS) >= 3) {
                    mStep++;
                    Activity forward = activity("Forward");
                    try {
                        forward.startActivityForResult(new Intent(forward, FinalActivity.class)
                                .addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT), 6);
                        log("Final started for result 6 with FLAG_ACTIVITY_FORWARD_RESULT");
                    } catch (AndroidRuntimeException e) {
                        log("FLAG_ACTIVITY_FORWARD_RESULT with result 6: " + e);
                    }
                    log("Forward starts Final with FLAG_ACTIVITY_FORWARD_RESULT and finishes");
                    forward.startActivity(new Intent(forward, FinalActivity.class)
                            .addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT));
                    forward.finish();
                }
                break;
            case 9:
                if (count("Final", GAINS) >= 1 && count("Forward", DESTROYS) >= 1) {
                    mStep++;
                    log("Final returns RESULT_OK and finishes");
                    activity("Final").setResult(Activity.RESULT_OK, result("Final"));
                    activity("Final").finish();
                }
                break;
            case 10:
                if (resumed("Main") && count("Final", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts Other");
                    activity("Main").startActivity(new Intent(activity("Main"), OtherActivity.class));
                }
                break;
            case 11:
                if (count("Other", GAINS) >= 1 && count("Main", STOPS) >= 4) {
                    mStep++;
                    log("Other starts OtherTop");
                    activity("Other").startActivity(new Intent(activity("Other"), OtherTopActivity.class));
                }
                break;
            case 12:
                if (count("OtherTop", GAINS) >= 1 && count("Other", STOPS) >= 1) {
                    mStep++;
                    log("OtherTop calls finishAffinity");
                    activity("OtherTop").finishAffinity();
                }
                break;
            case 13:
                if (resumed("Main") && count("Other", DESTROYS) >= 1 && count("OtherTop", DESTROYS) >= 1) {
                    mStep++;
                    log("Main starts Last");
                    activity("Main").startActivity(new Intent(activity("Main"), LastActivity.class));
                }
                break;
            case 14:
                if (count("Last", GAINS) >= 1 && count("Main", STOPS) >= 5) {
                    mStep++;
                    log("Last calls finishAndRemoveTask");
                    activity("Last").finishAndRemoveTask();
                }
                break;
            default:
                break;
        }
    }
}
