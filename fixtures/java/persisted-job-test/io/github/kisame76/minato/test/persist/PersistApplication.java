package io.github.kisame76.minato.test.persist;

import android.app.Application;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;

/**
 * Persisted jobs (the compatibility plan's decision 6): the first start schedules a persisted job (1) and one that is
 * not (2), each a minute away, and ends; the second start finds the persisted one still scheduled with what it asked
 * for, and cancels both. Android keeps a job that is not persisted across a process death, and drops it at a reboot,
 * so what it says about job 2 is shown only. tools/android-reference.sh ends the process between the starts with
 * `am kill` (a force-stop would cancel the jobs).
 */
public class PersistApplication extends Application {
    private static final String TAG = "persisted-job-test";
    private static final long LATENCY = 60_000;

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final JobScheduler scheduler = getSystemService(JobScheduler.class);
        final File marker = new File(getFilesDir(), "started");
        final ComponentName service = new ComponentName(this, NeverJob.class);
        if (!marker.exists()) {
            Log.i(TAG, "start 1");
            try {
                marker.createNewFile();
            } catch (java.io.IOException e) {
                Log.e(TAG, "marker: " + e);
            }
            Log.i(TAG, "job 1 scheduled: " + scheduler.schedule(new JobInfo.Builder(1, service)
                    .setMinimumLatency(LATENCY).setPersisted(true).build()));
            Log.i(TAG, "job 2 scheduled: " + scheduler.schedule(new JobInfo.Builder(2, service)
                    .setMinimumLatency(LATENCY).build()));
        } else {
            Log.i(TAG, "start 2");
            final JobInfo job1 = scheduler.getPendingJob(1);
            Log.i(TAG, "job 1 pending: " + (job1 != null));
            Log.i(TAG, "job 1 persisted: " + (job1 != null && job1.isPersisted()));
            Log.i(TAG, "job 1 latency kept: " + (job1 != null && job1.getMinLatencyMillis() == LATENCY));
            Log.i(TAG + "-device", "job 2 pending: " + (scheduler.getPendingJob(2) != null));
            scheduler.cancelAll();
            Log.i(TAG, "pending after cancelAll: " + scheduler.getAllPendingJobs().size());
        }
        Log.i(TAG, "done");
    }
}
