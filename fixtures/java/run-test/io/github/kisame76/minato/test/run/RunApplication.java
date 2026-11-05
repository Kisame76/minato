package io.github.kisame76.minato.test.run;

import android.app.AlarmManager;
import android.app.Application;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;

/**
 * Alarms and jobs that run (the compatibility plan's decision 6): alarms at +1 s (a PendingIntent), +2 s (the wall
 * clock), +3 s (a listener, exact) and a repeating one; jobs without a constraint, with a network, with an unmet
 * constraint and a deadline, and one cancelled before it runs. The live events are collected and written as one summary
 * after 9 s, so that the lines do not depend on the order in which two events a fraction of a second apart arrive;
 * each says whether it came within a window. Measured on apk-runner-35: AlarmManagerService raises every alarm that is
 * due in less than 5 s (MIN_FUTURITY) to 5 s, so all four alarms go off together at 5 s; a job without a constraint
 * starts within milliseconds, and one with an unmet constraint at its deadline. The repeating alarm's later firings lie
 * 60 s (the minimum interval) away, beyond this test; its first one is shown only.
 */
public class RunApplication extends Application {
    static final String TAG = "run-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.run";
    private static final long START = SystemClock.elapsedRealtime();
    private static final TreeMap<String, List<Long>> EVENTS = new TreeMap<>();
    private static final TreeMap<String, Boolean> FLAGS = new TreeMap<>();
    private static AlarmManager sAlarms;
    private static PendingIntent sRepeating;

    private static synchronized void record(String what) {
        EVENTS.computeIfAbsent(what, key -> new ArrayList<>()).add(SystemClock.elapsedRealtime() - START);
    }

    static void fired(String action) {
        record("alarm " + action);
        if (action.endsWith("REPEAT") && sAlarms != null) {
            sAlarms.cancel(sRepeating);
        }
    }

    static void jobStarted(int id, boolean deadlineExpired) {
        record("job " + id + " started");
        synchronized (RunApplication.class) {
            FLAGS.put("job " + id + " deadline expired", deadlineExpired);
        }
    }

    static void jobEnded(int id) {
        record("job " + id + " ended");
    }

    static void jobStopped(int id) {
        record("job " + id + " stopped");
    }

    private static synchronized int count(String what) {
        final List<Long> times = EVENTS.get(what);
        return times == null ? 0 : times.size();
    }

    /** The count of events and whether the first came between low and high milliseconds after the start. */
    private static synchronized String within(String what, long low, long high) {
        final List<Long> times = EVENTS.get(what);
        if (times == null || times.isEmpty()) {
            return "never";
        }
        final long first = times.get(0);
        return times.size() + " time(s), first " + (first >= low && first <= high ? "in time" : "out of time");
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private PendingIntent broadcast(int code, String action) {
        return PendingIntent.getBroadcast(this, code, new Intent(PACKAGE + "." + action)
                .setClass(this, AlarmReceiver.class), PendingIntent.FLAG_IMMUTABLE);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        log("start");
        final long elapsed = SystemClock.elapsedRealtime();
        final long wall = System.currentTimeMillis();
        sAlarms = (AlarmManager) getSystemService(Context.ALARM_SERVICE);
        sAlarms.set(AlarmManager.ELAPSED_REALTIME, elapsed + 1000, broadcast(1, "ELAPSED"));
        sAlarms.set(AlarmManager.RTC, wall + 2000, broadcast(2, "RTC"));
        sAlarms.setExact(AlarmManager.ELAPSED_REALTIME, elapsed + 3000, "run-test", () -> record("alarm LISTENER"),
                new Handler(Looper.getMainLooper()));
        sRepeating = broadcast(3, "REPEAT");
        sAlarms.setRepeating(AlarmManager.ELAPSED_REALTIME, elapsed + 1000, 1000, sRepeating);
        final PendingIntent cancelled = broadcast(4, "CANCELLED");
        sAlarms.set(AlarmManager.ELAPSED_REALTIME, elapsed + 2000, cancelled);
        sAlarms.cancel(cancelled);

        final JobScheduler jobs = (JobScheduler) getSystemService(Context.JOB_SCHEDULER_SERVICE);
        final ComponentName service = new ComponentName(this, TestJob.class);
        log("job 1 scheduled: " + jobs.schedule(new JobInfo.Builder(1, service).build()));
        log("job 2 scheduled: " + jobs.schedule(new JobInfo.Builder(2, service)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build()));
        log("job 3 scheduled: " + jobs.schedule(new JobInfo.Builder(3, service)
                .setRequiresDeviceIdle(true).setOverrideDeadline(3000).build()));
        log("job 4 scheduled: " + jobs.schedule(new JobInfo.Builder(4, service).setMinimumLatency(5000).build()));
        log("job 4 pending: " + (jobs.getPendingJob(4) != null));
        jobs.cancel(4);
        log("job 4 pending after cancel: " + (jobs.getPendingJob(4) != null));

        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            sAlarms.cancel(sRepeating);
            summary(jobs);
        }, 9000);
    }

    private void summary(JobScheduler jobs) {
        synchronized (RunApplication.class) {
            for (String what : EVENTS.keySet()) {
                Log.i(TAG + "-device", what + " at " + EVENTS.get(what) + " ms");
            }
        }
        log("alarm with a PendingIntent: " + within("alarm " + PACKAGE + ".ELAPSED", 4500, 6000));
        log("alarm on the wall clock: " + within("alarm " + PACKAGE + ".RTC", 4500, 6000));
        log("alarm with a listener: " + within("alarm LISTENER", 4500, 6000));
        // The first firing of a repeating alarm comes with the batch at 5 s on Android, but not every time (a cold
        // start of the emulator missed it once); the later ones are 60 s away. Only the cancel is compared.
        Log.i(TAG + "-device", "repeating alarm: " + within("alarm " + PACKAGE + ".REPEAT", 4500, 6000));
        log("repeating alarm after its cancel: " + (count("alarm " + PACKAGE + ".REPEAT") <= 1 ? "quiet" : "fired"));
        log("cancelled alarm: " + within("alarm " + PACKAGE + ".CANCELLED", 0, Long.MAX_VALUE));
        for (int id = 1; id <= 4; id++) {
            final long low = id == 3 ? 2500 : 0;
            final long high = id == 3 ? 4500 : 2000;
            final String line = "job " + id + ": " + within("job " + id + " started", low, high) + ", ended "
                    + count("job " + id + " ended") + ", stopped " + count("job " + id + " stopped")
                    + ", pending " + (jobs.getPendingJob(id) != null);
            // Job 2 asks for a network: it runs when the device has one, which differs by host.
            if (id == 2) {
                Log.i(TAG + "-device", line);
            } else {
                log(line);
            }
        }
        final ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        log("job 2 (network) ran exactly when a network was there: "
                + ((connectivity.getActiveNetwork() != null) == (count("job 2 started") > 0)));
        log("job 3 deadline expired: " + FLAGS.get("job 3 deadline expired"));
        log("done");
        System.exit(0);  // the process lives on otherwise: the receivers and the job service keep it
    }
}
