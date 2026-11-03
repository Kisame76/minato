package com.android.internal.minato;

import android.os.Handler;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Android's ANR report for a hung main Looper: Android's ActivityManager logs "ANR in <package>" when the app does not
 * answer in time and has ART dump every thread's stack (SIGQUIT). Minato has no system server, so this thread of the
 * app asks the main Looper every 5 s; when a question stays unanswered for 5 s it logs the ANR lines under
 * ActivityManager and every thread's Java stack in the layout of ART's thread dump, once per hang. ART's own SIGQUIT
 * dump is not used: the guest's ART runs no signal catcher, so a SIGQUIT does nothing. A wait the watchdog itself
 * overslept by more than a second (the process suspended, as iOS does in the background) counts as no answer due.
 */
public final class AnrWatchdog extends Thread {
    private static final String TAG = "ActivityManager";
    private static final long TIMEOUT_MS = 5000;
    private static final long OVERSLEEP_MS = 1000;

    private final Handler mMain;
    private final String mPackage;
    private volatile boolean mAnswered;

    private AnrWatchdog(Handler main, String packageName) {
        super("MinatoAnrWatchdog");
        setDaemon(true);
        mMain = main;
        mPackage = packageName;
    }

    /** Starts the watchdog of the main Looper, whose Handler `main` is, for the app `packageName`. */
    public static void start(Handler main, String packageName) {
        new AnrWatchdog(main, packageName).start();
    }

    @Override
    public void run() {
        boolean reported = false;
        while (true) {
            mAnswered = false;
            mMain.post(() -> mAnswered = true);
            long asked = SystemClock.uptimeMillis();
            SystemClock.sleep(TIMEOUT_MS);
            if (SystemClock.uptimeMillis() - asked > TIMEOUT_MS + OVERSLEEP_MS) {
                continue;
            }
            if (mAnswered) {
                reported = false;
                continue;
            }
            if (!reported) {
                report();
                reported = true;
            }
            // The next question once the main Looper has answered this one.
            while (!mAnswered) {
                SystemClock.sleep(TIMEOUT_MS / 10);
            }
            reported = false;
        }
    }

    private void report() {
        int pid = Process.myPid();
        Log.e(TAG, "ANR in " + mPackage);
        Log.e(TAG, "PID: " + pid);
        Log.e(TAG, "Reason: the main Looper did not answer for " + TIMEOUT_MS / 1000 + " s");
        Log.i(TAG, "----- pid " + pid + " at "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(new Date()) + " -----");
        Log.i(TAG, "Cmd line: " + mPackage);
        List<Map.Entry<Thread, StackTraceElement[]>> threads = new ArrayList<>(Thread.getAllStackTraces().entrySet());
        // The main thread first, as ART's dump has it.
        threads.sort((a, b) -> Boolean.compare(b.getKey() == mMain.getLooper().getThread(),
                a.getKey() == mMain.getLooper().getThread()));
        for (Map.Entry<Thread, StackTraceElement[]> entry : threads) {
            Thread thread = entry.getKey();
            Log.i(TAG, "");
            Log.i(TAG, "\"" + thread.getName() + "\"" + (thread.isDaemon() ? " daemon" : "") + " prio="
                    + thread.getPriority() + " tid=" + thread.getId() + " " + state(thread.getState()));
            for (StackTraceElement frame : entry.getValue()) {
                Log.i(TAG, "  at " + frame);
            }
        }
        Log.i(TAG, "----- end " + pid + " -----");
    }

    /** ART's names of the thread states. */
    private static String state(Thread.State state) {
        switch (state) {
            case NEW: return "Starting";
            case RUNNABLE: return "Runnable";
            case BLOCKED: return "Blocked";
            case WAITING: return "Waiting";
            case TIMED_WAITING: return "TimedWaiting";
            default: return "Terminated";
        }
    }
}
