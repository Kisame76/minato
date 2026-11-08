package io.github.kisame76.minato.test.jobsalarms;

import android.app.AlarmManager;
import android.app.Application;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Intent;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.PersistableBundle;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;

/**
 * Alarms and jobs that run, beyond run-test's (gap 9 of the compatibility plan). Alarms of an app that holds
 * USE_EXACT_ALARM: exact, exact and allowed while idle, an alarm clock with its next-alarm info, a window, one with a
 * listener on a handler thread, one cancelled; jobs: an expedited one with its params, a regular one, one rescheduled by
 * its linear backoff, one cancelled while it runs (onStopJob and its stop reason), one with work items, and the
 * charging, battery-not-low and storage-not-low constraints against what the device's battery says. The live events
 * are collected and written as one summary after 26 s, each alarm with whether it came within a window; the alarms
 * are scheduled for 6 to 9 s, as AlarmManagerService raises every alarm that is due in less than 5 s to 5 s
 * (MIN_FUTURITY).
 */
public class JobsAlarmsApplication extends Application {
    static final String TAG = "jobs-alarms-run-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.jobsalarms";
    private static final long START = SystemClock.elapsedRealtime();
    private static final TreeMap<String, List<Long>> EVENTS = new TreeMap<>();
    private static final TreeMap<String, String> FACTS = new TreeMap<>();

    static synchronized void record(String what) {
        EVENTS.computeIfAbsent(what, key -> new ArrayList<>()).add(SystemClock.elapsedRealtime() - START);
    }

    static synchronized void fact(String what) {
        final int split = what.indexOf(':') >= 0 ? what.indexOf(':') : what.indexOf(" stop reason");
        FACTS.put(split > 0 ? what.substring(0, split) : what, what);
    }

    static synchronized int count(String what) {
        final List<Long> times = EVENTS.get(what);
        return times == null ? 0 : times.size();
    }

    private static synchronized String within(String what, long low, long high) {
        final List<Long> times = EVENTS.get(what);
        if (times == null || times.isEmpty()) {
            return "never";
        }
        final long first = times.get(0);
        return times.size() + " time(s), first " + (first >= low && first <= high ? "in time" : "out of time");
    }

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private AlarmManager mAlarms;
    private JobScheduler mJobs;

    @Override
    public void onCreate() {
        super.onCreate();
        mMain.post(this::start);
    }

    private PendingIntent intent(String action) {
        return PendingIntent.getBroadcast(this, 0, new Intent(action).setClassName(PACKAGE, PACKAGE + ".AlarmReceiver"),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private void start() {
        Log.i(TAG, "start");
        mAlarms = getSystemService(AlarmManager.class);
        mJobs = getSystemService(JobScheduler.class);
        final long now = SystemClock.elapsedRealtime();
        final long wall = System.currentTimeMillis();
        Log.i(TAG, "can schedule exact alarms: " + mAlarms.canScheduleExactAlarms());
        try {
            mAlarms.setExact(AlarmManager.RTC_WAKEUP, wall + 6000, intent("EXACT"));
            mAlarms.setExactAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, now + 7000, intent("EXACT_IDLE"));
            mAlarms.setWindow(AlarmManager.ELAPSED_REALTIME, now + 6000, 3000, intent("WINDOW"));
            final AlarmManager.AlarmClockInfo info = new AlarmManager.AlarmClockInfo(wall + 8000, intent("SHOW"));
            mAlarms.setAlarmClock(info, intent("CLOCK"));
            final AlarmManager.AlarmClockInfo next = mAlarms.getNextAlarmClock();
            Log.i(TAG, "next alarm clock: trigger " + (next != null && next.getTriggerTime() == wall + 8000)
                    + ", show intent " + (next != null && next.getShowIntent() != null));
            final HandlerThread thread = new HandlerThread("alarm-thread");
            thread.start();
            mAlarms.setExact(AlarmManager.ELAPSED_REALTIME, now + 9000, "listener-tag", () -> {
                record("alarm listener");
                fact("alarm listener: on the handler's thread " + (Looper.myLooper() == thread.getLooper()));
            }, new Handler(thread.getLooper()));
            mAlarms.setExact(AlarmManager.RTC, wall + 6000, intent("CANCELLED"));
            mAlarms.cancel(intent("CANCELLED"));
            Log.i(TAG, "alarms set");
        } catch (RuntimeException e) {
            Log.i(TAG, "alarms: throws " + e.getClass().getName());
        }
        schedule();
        mMain.postDelayed(this::cancelFour, 2000);
        mMain.postDelayed(this::summary, 26000);
    }

    private JobInfo.Builder job(int id) {
        return new JobInfo.Builder(id, new ComponentName(this, TestJob.class));
    }

    private void schedule() {
        final PersistableBundle extras = new PersistableBundle();
        extras.putString("key", "extra");
        final android.os.Bundle transientExtras = new android.os.Bundle();
        transientExtras.putString("key", "transient");
        Log.i(TAG, "job 1 expedited: " + mJobs.schedule(job(1).setExpedited(true).setExtras(extras)
                .setTransientExtras(transientExtras).build()));
        Log.i(TAG, "job 2: " + mJobs.schedule(job(2).build()));
        Log.i(TAG, "job 3 with a backoff: " + mJobs.schedule(job(3)
                .setBackoffCriteria(JobInfo.MIN_BACKOFF_MILLIS, JobInfo.BACKOFF_POLICY_LINEAR).build()));
        Log.i(TAG, "job 4: " + mJobs.schedule(job(4).build()));
        final int enqueued = mJobs.enqueue(job(5).build(), new JobWorkItem(new Intent("one")));
        mJobs.enqueue(job(5).build(), new JobWorkItem(new Intent("two")));
        mJobs.enqueue(job(5).build(), new JobWorkItem(new Intent("three")));
        Log.i(TAG, "job 5 with three work items: " + enqueued);
        Log.i(TAG, "job 6 charging: " + mJobs.schedule(job(6).setRequiresCharging(true).build()));
        Log.i(TAG, "job 7 battery not low: " + mJobs.schedule(job(7).setRequiresBatteryNotLow(true).build()));
        Log.i(TAG, "job 8 storage not low: " + mJobs.schedule(job(8).setRequiresStorageNotLow(true).build()));
    }

    /** Job 4 runs until cancelled: cancel it once it has started. */
    private void cancelFour() {
        if (count("job 4 started") > 0) {
            mJobs.cancel(4);
        }
    }

    private void summary() {
        // Alarms scheduled at 6 to 9 s may arrive up to 3 s late; MIN_FUTURITY raised nothing here.
        Log.i(TAG, "alarm EXACT: " + within("alarm EXACT", 5500, 9500));
        Log.i(TAG, "alarm EXACT_IDLE: " + within("alarm EXACT_IDLE", 6500, 10500));
        Log.i(TAG, "alarm WINDOW: " + within("alarm WINDOW", 5500, 12500));
        Log.i(TAG, "alarm CLOCK: " + within("alarm CLOCK", 7500, 11500));
        Log.i(TAG, "alarm listener: " + within("alarm listener", 8500, 12500));
        Log.i(TAG, "alarm CANCELLED: " + within("alarm CANCELLED", 0, 1000000));
        Log.i(TAG, "next alarm clock after it rang: " + mAlarms.getNextAlarmClock());
        synchronized (JobsAlarmsApplication.class) {
            for (String fact : FACTS.values()) {
                Log.i(TAG, fact);
            }
            for (String key : EVENTS.keySet()) {
                if (key.startsWith("work ")) {
                    Log.i(TAG, key + " " + EVENTS.get(key).size() + " time(s)");
                }
            }
        }
        for (int id = 1; id <= 5; id++) {
            if (id == 3) {
                continue;  // its second run came at 10 s on one run of the emulator and not on another: shown below
            }
            Log.i(TAG, "job " + id + ": started " + count("job " + id + " started") + " time(s), stopped "
                    + count("job " + id + " stopped") + " time(s)");
        }
        // The rescheduled job came again after its 10 s backoff on one run of the emulator and not within 16 s of the
        // first run on another (JobSchedulerService may hold the jobs of an app that is not in the foreground until
        // several are ready [inferred]): shown, not compared.
        Log.i(TAG + "-device", "job 3 started " + count("job 3 started") + " time(s), gap " + backoffGap());
        final BatteryManager battery = getSystemService(BatteryManager.class);
        final boolean charging = battery.isCharging();
        final int level = battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
        // On the emulator, plugged in since boot, job 6 did not run within the 26 s: Android 15 seems to wait until the
        // device has charged for a while before it counts as charging for a job [inferred], so only that the job never
        // runs uncharged is compared.
        Log.i(TAG, "job 6 never ran while the device was not charging: " + (count("job 6 started") == 0 || charging));
        Log.i(TAG, "job 7 ran exactly when the battery is not low: " + ((count("job 7 started") > 0) == (level > 15)));
        final android.content.Intent sticky = registerReceiver(null,
                new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
        Log.i(TAG + "-device", "plugged " + sticky.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) + ", status "
                + sticky.getIntExtra(BatteryManager.EXTRA_STATUS, -1));
        Log.i(TAG + "-device", "charging " + charging + ", level " + level + ", job 6 ran " + count("job 6 started")
                + ", job 7 ran " + count("job 7 started") + ", job 8 ran " + count("job 8 started"));
        Log.i(TAG, "done");
    }

    private static synchronized long backoffGap() {
        final List<Long> times = EVENTS.get("job 3 started");
        return times == null || times.size() < 2 ? -1 : times.get(1) - times.get(0);
    }
}
