package io.github.kisame76.minato.test.jobwakeups;

import android.app.Application;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Method;

/**
 * What the job scheduler costs while it only waits: one job that is not due for an hour is pending, the app does
 * nothing for 60 s, and the log says how often the scheduler's runner woke up in that time. The count is Minato's
 * (android.app.LocalJobScheduler.wakeupCount, read by reflection) and only shown, in the -device line; Android's log
 * has the same other lines.
 */
public class WakeupsApplication extends Application {
    private static final String TAG = "job-wakeups-test";

    private static long wakeups() {
        try {
            final Method method = Class.forName("android.app.LocalJobScheduler").getDeclaredMethod("wakeupCount");
            method.setAccessible(true);
            return (Long) method.invoke(null);
        } catch (ReflectiveOperationException | RuntimeException e) {
            return -1;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::start);
    }

    private void start() {
        final JobScheduler jobs = getSystemService(JobScheduler.class);
        final JobInfo job = new JobInfo.Builder(1, new ComponentName(this, TestJob.class))
                .setMinimumLatency(3600 * 1000).build();
        Log.i(TAG, "scheduled: " + jobs.schedule(job));
        final long before = wakeups();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            final long after = wakeups();
            Log.i(TAG + "-device", "wakeups in 60 s: " + (before < 0 ? "no counter" : String.valueOf(after - before)));
            Log.i(TAG, "pending after 60 s: " + (jobs.getPendingJob(1) != null));
            Log.i(TAG, "done");
        }, 60000);
    }
}
