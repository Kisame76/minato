package io.github.kisame76.minato.test.jobs;

import android.app.ActivityManager;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.PersistableBundle;
import android.os.UserManager;
import android.util.Log;

import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.TimeUnit;

/**
 * The job scheduler as an app uses it (the Unity test APK probe's WorkManager): jobs scheduled, replaced, looked up and
 * cancelled, in namespaces, with work items, with their constraints; the refusals and their messages, the limit of
 * distinct jobs, and the other calls that WorkManager's start makes (an unset pending intent, the exit reasons, the
 * user's unlock, a component's enabled state). Every job waits for an hour, so none runs while the test does.
 */
public class JobsApplication extends Application {
    static final String TAG = "jobs-test";
    static final String PACKAGE = "io.github.kisame76.minato.test.jobs";
    static final long HOUR = TimeUnit.HOURS.toMillis(1);

    private JobScheduler mScheduler;
    private ComponentName mWork;
    private ComponentName mOpen;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private JobInfo.Builder builder(int id) {
        return new JobInfo.Builder(id, mWork).setMinimumLatency(HOUR);
    }

    private static String ids(List<JobInfo> jobs) {
        TreeSet<Integer> ids = new TreeSet<>();
        if (jobs != null) {
            for (JobInfo job : jobs) {
                ids.add(job.getId());
            }
        }
        return ids.toString();
    }

    private static String describe(JobInfo job) {
        if (job == null) {
            return "null";
        }
        PersistableBundle extras = job.getExtras();
        return "id " + job.getId() + ", " + job.getService().getShortClassName() + ", latency "
                + job.getMinLatencyMillis() + ", deadline " + job.getMaxExecutionDelayMillis() + ", charging "
                + job.isRequireCharging() + ", idle " + job.isRequireDeviceIdle() + ", battery not low "
                + job.isRequireBatteryNotLow() + ", storage not low " + job.isRequireStorageNotLow() + ", network "
                + job.getNetworkType() + ", persisted " + job.isPersisted() + ", periodic " + job.isPeriodic() + " "
                + (job.isPeriodic() ? job.getIntervalMillis() + "/" + job.getFlexMillis() : "-") + ", backoff "
                + job.getBackoffPolicy() + "/" + job.getInitialBackoffMillis() + ", expedited " + job.isExpedited()
                + ", extras " + extras.size() + " " + extras.getString("name") + " " + extras.getLong("count");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mScheduler = (JobScheduler) getSystemService(Context.JOB_SCHEDULER_SERVICE);
        mWork = new ComponentName(this, WorkJob.class);
        mOpen = new ComponentName(this, OpenJob.class);
        log("the service: " + (mScheduler == null ? "null" : mScheduler.getClass().getName()) + ", namespace "
                + mScheduler.getNamespace());
        mScheduler.cancelInAllNamespaces();
        scheduling();
        namespaces();
        workItems();
        refusals();
        limit();
        startOfWorkManager();
        mScheduler.cancelInAllNamespaces();
        log("done");
    }

    private void scheduling() {
        log("schedule: " + mScheduler.schedule(builder(1).build()) + ", pending " + ids(mScheduler.getAllPendingJobs()));
        log("pending job 1: " + describe(mScheduler.getPendingJob(1)));
        log("schedule 1 again, later: " + mScheduler.schedule(new JobInfo.Builder(1, mWork)
                .setMinimumLatency(2 * HOUR).setOverrideDeadline(3 * HOUR).build())
                + ", pending " + ids(mScheduler.getAllPendingJobs()));
        log("pending job 1: " + describe(mScheduler.getPendingJob(1)));
        log("schedule 2: " + mScheduler.schedule(builder(2).build()) + ", pending "
                + ids(mScheduler.getAllPendingJobs()));
        log("an unknown job: " + describe(mScheduler.getPendingJob(99)));
        mScheduler.cancel(1);
        log("cancel 1: pending " + ids(mScheduler.getAllPendingJobs()));
        mScheduler.cancel(99);
        log("cancel 99: pending " + ids(mScheduler.getAllPendingJobs()));
        mScheduler.cancelAll();
        log("cancelAll: pending " + ids(mScheduler.getAllPendingJobs()) + ", a list " + (mScheduler.getAllPendingJobs() != null));
        PersistableBundle extras = new PersistableBundle();
        extras.putString("name", "Minato");
        extras.putLong("count", 42L);
        log("every constraint: " + mScheduler.schedule(new JobInfo.Builder(3, mWork).setMinimumLatency(HOUR)
                .setOverrideDeadline(2 * HOUR).setRequiresCharging(true).setRequiresDeviceIdle(true)
                .setRequiresBatteryNotLow(true).setRequiresStorageNotLow(true)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_NONE).setExtras(extras).build()));
        log("pending job 3: " + describe(mScheduler.getPendingJob(3)));
        log("a backoff with the idle constraint: " + outcome(() -> new JobInfo.Builder(3, mWork)
                .setRequiresDeviceIdle(true).setBackoffCriteria(30000L, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build()));
        log("a backoff: " + mScheduler.schedule(builder(12).setExtras(extras)
                .setBackoffCriteria(30000L, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build()));
        log("pending job 12: " + describe(mScheduler.getPendingJob(12)));
        log("a periodic job: " + mScheduler.schedule(new JobInfo.Builder(4, mWork)
                .setPeriodic(TimeUnit.MINUTES.toMillis(20), TimeUnit.MINUTES.toMillis(5)).build()));
        log("pending job 4: " + describe(mScheduler.getPendingJob(4)));
        log("a periodic job below the minimum: " + mScheduler.schedule(new JobInfo.Builder(5, mWork)
                .setPeriodic(1000L, 1000L).build()));
        log("pending job 5: " + describe(mScheduler.getPendingJob(5)));
        log("an expedited job with a constraint it cannot have: " + outcome(() -> new JobInfo.Builder(6, mWork)
                .setExpedited(true).setRequiresCharging(true).build()));
        mScheduler.schedule(builder(2).build());
        log("the reason of a job that waits for its delay: " + mScheduler.getPendingJobReason(2) + ", of a periodic one "
                + mScheduler.getPendingJobReason(4) + ", of an unknown one " + mScheduler.getPendingJobReason(99));
        mScheduler.cancelAll();
    }

    private void namespaces() {
        JobScheduler a = mScheduler.forNamespace("a");
        JobScheduler b = mScheduler.forNamespace("b");
        log("namespaces: " + a.getNamespace() + " " + b.getNamespace() + " " + mScheduler.getNamespace());
        log("schedule 1 in a, b and the default: " + a.schedule(builder(1).build()) + " " + b.schedule(builder(1).build())
                + " " + mScheduler.schedule(builder(1).build()) + ", then 2 in a: " + a.schedule(builder(2).build()));
        log("pending in a " + ids(a.getAllPendingJobs()) + ", b " + ids(b.getAllPendingJobs()) + ", default "
                + ids(mScheduler.getAllPendingJobs()));
        Map<String, List<JobInfo>> all = new TreeMap<>();
        Map<String, List<JobInfo>> pending = mScheduler.getPendingJobsInAllNamespaces();
        for (Map.Entry<String, List<JobInfo>> entry : pending.entrySet()) {
            all.put(String.valueOf(entry.getKey()), entry.getValue());
        }
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, List<JobInfo>> entry : all.entrySet()) {
            text.append(entry.getKey()).append('=').append(ids(entry.getValue())).append(' ');
        }
        log("in all namespaces: " + text.toString().trim());
        b.cancel(1);
        log("cancel 1 in b: a " + ids(a.getAllPendingJobs()) + ", b " + ids(b.getAllPendingJobs()));
        a.cancelAll();
        log("cancelAll in a: a " + ids(a.getAllPendingJobs()) + ", default " + ids(mScheduler.getAllPendingJobs()));
        a.schedule(builder(7).build());
        b.schedule(builder(8).build());
        mScheduler.cancelInAllNamespaces();
        log("cancelInAllNamespaces: " + pendingEverywhere());
        log("an empty namespace: " + outcome(() -> mScheduler.forNamespace("")));
        log("a null namespace: " + outcome(() -> mScheduler.forNamespace(null)));
        log("a namespace of 1001 characters: " + outcome(() -> mScheduler.forNamespace(new String(new char[1001])
                .replace('\0', 'n')).schedule(builder(1).build())));
        mScheduler.cancelInAllNamespaces();
    }

    private String pendingEverywhere() {
        Map<String, List<JobInfo>> pending = mScheduler.getPendingJobsInAllNamespaces();
        int count = 0;
        for (List<JobInfo> jobs : pending.values()) {
            count += jobs.size();
        }
        return "namespaces " + pending.size() + ", jobs " + count;
    }

    private void workItems() {
        Intent first = new Intent("first");
        log("enqueue: " + mScheduler.enqueue(builder(10).build(), new JobWorkItem(first)) + ", pending "
                + ids(mScheduler.getAllPendingJobs()));
        log("enqueue the same job: " + mScheduler.enqueue(builder(10).build(), new JobWorkItem(new Intent("second")))
                + ", pending " + ids(mScheduler.getAllPendingJobs()));
        log("enqueue a changed job: " + mScheduler.enqueue(new JobInfo.Builder(10, mWork)
                .setMinimumLatency(2 * HOUR).build(), new JobWorkItem(new Intent("third")))
                + ", " + describe(mScheduler.getPendingJob(10)));
        log("enqueue without work: " + outcome(() -> mScheduler.enqueue(builder(11).build(), null)));
        log("a work item: " + new JobWorkItem(first).getIntent().getAction() + ", delivery count "
                + new JobWorkItem(first).getDeliveryCount());
        mScheduler.cancelAll();
    }

    private void refusals() {
        log("a service of the app, not found: " + outcome(() -> mScheduler.schedule(new JobInfo.Builder(20,
                new ComponentName(PACKAGE, PACKAGE + ".Missing")).setMinimumLatency(HOUR).build())));
        log("a service of another package: " + outcome(() -> mScheduler.schedule(new JobInfo.Builder(21,
                new ComponentName("com.example.other", "com.example.other.Job")).setMinimumLatency(HOUR).build())));
        log("a service without BIND_JOB_SERVICE: " + outcome(() -> mScheduler.schedule(new JobInfo.Builder(22, mOpen)
                .setMinimumLatency(HOUR).build())));
        log("a persisted job without RECEIVE_BOOT_COMPLETED: " + outcome(() -> mScheduler.schedule(builder(23)
                .setPersisted(true).build())));
        log("a job with neither: " + outcome(() -> new JobInfo.Builder(26, mWork).build()));
        log("a periodic job with a delay: " + outcome(() -> new JobInfo.Builder(27, mWork)
                .setPeriodic(HOUR).setMinimumLatency(HOUR).build()));
        log("a negative delay: " + outcome(() -> new JobInfo.Builder(28, mWork).setMinimumLatency(-1).build()));
        log("pending after the refusals: " + ids(mScheduler.getAllPendingJobs()));
        log("can run user-initiated jobs: " + mScheduler.canRunUserInitiatedJobs());
    }

    private void limit() {
        int first = 0;
        String message = null;
        int scheduled = 0;
        for (int id = 1; id <= 160; id++) {
            try {
                mScheduler.schedule(builder(id).build());
                scheduled++;
            } catch (IllegalStateException e) {
                first = id;
                message = e.getMessage();
                break;
            }
        }
        log("the limit: " + scheduled + " scheduled, refused at " + first + ": " + message);
        log("pending at the limit: " + mScheduler.getAllPendingJobs().size() + ", job 1 again: "
                + outcome(() -> mScheduler.schedule(builder(1).build())));
        mScheduler.cancelAll();
        log("pending after cancelAll: " + mScheduler.getAllPendingJobs().size());
    }

    // What WorkManager's start asks besides the scheduler (ForceStopRunnable, Schedulers).
    private void startOfWorkManager() {
        Intent intent = new Intent().setComponent(new ComponentName(this, WorkJob.class))
                .setAction("ACTION_FORCE_STOP_RESCHEDULE");
        PendingIntent pending = PendingIntent.getBroadcast(this, -1, intent,
                PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_MUTABLE);
        log("an unset pending intent: " + pending);
        ActivityManager activityManager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        List<ApplicationExitInfo> exits = activityManager.getHistoricalProcessExitReasons(null, 0, 0);
        log("the exit reasons: " + (exits != null));
        log("the user is unlocked: " + ((UserManager) getSystemService(Context.USER_SERVICE)).isUserUnlocked());
        PackageManager packageManager = getPackageManager();
        log("a component's state: " + packageManager.getComponentEnabledSetting(mWork));
        packageManager.setComponentEnabledSetting(mWork, PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP);
        log("enabled: " + packageManager.getComponentEnabledSetting(mWork));
        packageManager.setComponentEnabledSetting(mWork, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP);
        log("disabled: " + packageManager.getComponentEnabledSetting(mWork));
        packageManager.setComponentEnabledSetting(mWork, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP);
        log("back to the default: " + packageManager.getComponentEnabledSetting(mWork));
        ComponentName missing = new ComponentName(PACKAGE, PACKAGE + ".Missing");
        log("an unknown component, read: " + outcome(() -> packageManager.getComponentEnabledSetting(missing)));
        log("an unknown component, set: " + outcome(() -> {
            packageManager.setComponentEnabledSetting(missing, PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                    PackageManager.DONT_KILL_APP);
            return "fine";
        }));
        log("an unknown package: " + outcome(() -> packageManager.getComponentEnabledSetting(
                new ComponentName("com.example.other", "com.example.other.Job"))));
        log("an activity of the app: " + packageManager.getComponentEnabledSetting(
                new ComponentName("io.github.kisame76.minato.test.jobs", "android.app.Activity")));
        // A service the manifest disables (Google's asset-delivery library enables its extraction service this way):
        // PackageManagerService looks it up with MATCH_DISABLED_COMPONENTS, so the state can be set and read.
        ComponentName off = new ComponentName(PACKAGE, PACKAGE + ".OffJob");
        log("a service the manifest disables, found by default: "
                + outcome(() -> packageManager.getServiceInfo(off, 0).name));
        log("... with MATCH_DISABLED_COMPONENTS, enabled=" + outcome(
                () -> packageManager.getServiceInfo(off, PackageManager.MATCH_DISABLED_COMPONENTS).enabled));
        log("its state: " + packageManager.getComponentEnabledSetting(off));
        log("set enabled: " + outcome(() -> {
            packageManager.setComponentEnabledSetting(off, PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                    PackageManager.DONT_KILL_APP);
            return packageManager.getComponentEnabledSetting(off);
        }));
        log("set back to the default: " + outcome(() -> {
            packageManager.setComponentEnabledSetting(off, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    PackageManager.DONT_KILL_APP);
            return packageManager.getComponentEnabledSetting(off);
        }));
    }
}
