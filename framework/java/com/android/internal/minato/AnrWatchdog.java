package com.android.internal.minato;

import android.os.Handler;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

/**
 * Android's ANR report for a hung main Looper: Android's ActivityManager logs "ANR in <package>" when the app does not
 * answer in time and has ART dump every thread's stack (SIGQUIT). Minato has no system server, so this thread of the
 * app asks the main Looper every 5 s; when a question stays unanswered for 5 s it logs the ANR lines under
 * ActivityManager and sends the process SIGQUIT, once per hang: ART's signal catcher dumps every thread, and the
 * palette logs the dump (Android writes it to /data/anr). A wait the watchdog itself overslept by more than a second
 * (the process suspended, as iOS does in the background) counts as no answer due.
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
        Process.sendSignal(pid, Process.SIGNAL_QUIT);
    }
}
