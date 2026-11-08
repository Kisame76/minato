package io.github.kisame76.minato.test.jobconstraints;

import android.app.Application;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * Jobs that lose a constraint while they run (gap 9 of the compatibility plan): job 1 needs the device to charge, job
 * 2 and job 3 a battery that is not low (jobs 4 to 8 only make five jobs ready). The test's host unplugs the device
 * at the start and plugs it in once jobs 2 and 3 run; after "ready" it unplugs it, takes the level to 10 %, plugs it
 * in and takes it to 50 %. A job stopped for the lost constraint gets onStopJob with its stop reason and, if it
 * returns true, is started again once the constraint holds; job 3 returns false and is not.
 */
public class ConstraintsApplication extends Application {
    static final String TAG = "job-constraints-test";
    private static final List<String> LOG = new ArrayList<>();
    private static final List<List<String>> PER_JOB = new ArrayList<>();

    static {
        for (int id = 0; id <= 8; id++) {
            PER_JOB.add(new ArrayList<>());
        }
    }

    /** One job's event in its own list, so that the summary does not depend on the order the jobs ran in. */
    static synchronized void event(int id, String what) {
        PER_JOB.get(id).add(what);
        LOG.add("job " + id + " " + what);
        Log.i(TAG + "-live", "job " + id + " " + what);
    }

    static synchronized int count(String what) {
        int count = 0;
        for (String line : LOG) {
            if (line.startsWith(what)) {
                count++;
            }
        }
        return count;
    }

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private JobScheduler mJobs;
    private boolean mReady;
    private boolean mTwoRun;
    private long mReadyAt;

    @Override
    public void onCreate() {
        super.onCreate();
        mMain.post(this::start);
    }

    private JobInfo.Builder job(int id) {
        return new JobInfo.Builder(id, new ComponentName(this, TestJob.class));
    }

    private void start() {
        mJobs = getSystemService(JobScheduler.class);
        Log.i(TAG, "job 1 charging: " + mJobs.schedule(job(1).setRequiresCharging(true).build()));
        Log.i(TAG, "job 2 battery not low: " + mJobs.schedule(job(2).setRequiresBatteryNotLow(true).build()));
        Log.i(TAG, "job 3 battery not low: " + mJobs.schedule(job(3).setRequiresBatteryNotLow(true).build()));
        // Jobs 4 to 8 have no constraint and finish at once: JobSchedulerService may hold the jobs of an app that is
        // not in the foreground until five are ready [inferred], so that the three above would wait.
        for (int id = 4; id <= 8; id++) {
            Log.i(TAG, "job " + id + ": " + mJobs.schedule(job(id).build()));
        }
        mMain.postDelayed(this::poll, 200);
    }

    /**
     * "jobs 2 and 3 run" once the jobs that need no charging have started; "ready" once job 1 has started too, which
     * waits for the device to charge (the test's host plugs it in after the first line); the events before "ready" are
     * dropped, so that the summary only has what the battery's steps cause. "done" 3 s after jobs 1 and 2 have started
     * again, or 25 s after "ready".
     */
    private void poll() {
        if (!mTwoRun && count("job 2 started") > 0 && count("job 3 started") > 0) {
            mTwoRun = true;
            Log.i(TAG, "jobs 2 and 3 run");
        }
        if (mTwoRun && !mReady && count("job 1 started") > 0) {
            mReady = true;
            synchronized (ConstraintsApplication.class) {
                for (List<String> events : PER_JOB) {
                    events.clear();
                }
                LOG.clear();
            }
            mReadyAt = SystemClock.elapsedRealtime();
            Log.i(TAG, "ready");
        }
        if (mReady && (SystemClock.elapsedRealtime() - mReadyAt > 25000 || count("job 1 started") > 0
                && count("job 1 stopped") > 0 && count("job 2 started") > 0 && count("job 2 stopped") > 0)) {
            mMain.postDelayed(this::summary, 3000);
            return;
        }
        mMain.postDelayed(this::poll, 200);
    }

    private void summary() {
        for (int id = 1; id <= 8; id++) {
            synchronized (ConstraintsApplication.class) {
                Log.i(TAG, "job " + id + ": " + PER_JOB.get(id));
            }
        }
        Log.i(TAG, "done");
    }
}
