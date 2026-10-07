/*
 * Interim (ADR-0039 step a): the getters of Android 15's android.os.Process that the app process needs before the
 * framework's own build, with their signatures from the tag's class; is64Bit for b3.1's Build. AOSP's Process replaces
 * it when framework.jar is built from the tag's sources (step b3/c): its sources pull in most of the framework, 80,799
 * javac errors on their own. Step c1 adds what UserHandle, HandlerThread, StrictMode and ContextImpl call, step c2c3
 * what Instrumentation calls, and the corpus harness's Displayed line the start times, which apps read too.
 */
package android.os;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import dalvik.system.VMRuntime;

public class Process {
    /**
     * The root uid, which holds every permission (step c1's ContextImpl).
     *
     * @hide
     */
    public static final int ROOT_UID = 0;

    /** The system's uid, which holds every permission too. */
    public static final int SYSTEM_UID = 1000;

    /**
     * As AOSP's: no pid, which an AttributionSource without one carries (ADR-0045, gap 13a).
     *
     * @hide
     */
    public static final int INVALID_PID = -1;

    // The uid ranges and gids of the tag's Process, which AOSP's UserHandle reads (step c1).

    /** @hide */
    public static final int SHARED_USER_GID = 9997;

    public static final int FIRST_APPLICATION_UID = 10000;

    public static final int LAST_APPLICATION_UID = 19999;

    /** @hide */
    public static final int FIRST_APPLICATION_CACHE_GID = 20000;

    /** @hide */
    public static final int FIRST_SDK_SANDBOX_UID = 20000;

    /** @hide */
    public static final int LAST_SDK_SANDBOX_UID = 29999;

    /** @hide */
    public static final int FIRST_SHARED_APPLICATION_GID = 50000;

    /** @hide */
    public static final int FIRST_APP_ZYGOTE_ISOLATED_UID = 90000;

    /** @hide */
    public static final int LAST_APP_ZYGOTE_ISOLATED_UID = 98999;

    /** @hide */
    public static final int FIRST_ISOLATED_UID = 99000;

    /** @hide */
    public static final int LAST_ISOLATED_UID = 99999;

    /** The tag's priorities that HandlerThread and QueuedWork's thread take (step c1). */
    public static final int THREAD_PRIORITY_DEFAULT = 0;

    public static final int THREAD_PRIORITY_FOREGROUND = -2;

    /** As AOSP's: a background thread's priority, which AsyncTask's workers take. */
    public static final int THREAD_PRIORITY_BACKGROUND = 10;

    /** The app's process ID, as getpid() answers. */
    public static final int myPid() {
        return Os.getpid();
    }

    /** The calling thread's ID, as gettid() answers. */
    public static final int myTid() {
        return Os.gettid();
    }

    /** The app's uid, as getuid() answers (ADR-0039 decision 3). */
    public static final int myUid() {
        return Os.getuid();
    }

    /** The app's user, as the tag's Process makes it, which Context.getUser returns (step c1). */
    public static UserHandle myUserHandle() {
        return UserHandle.of(UserHandle.getUserId(myUid()));
    }

    /** Whether the uid is an isolated process's, as the tag's isIsolated tells it. Minato's app never is. */
    public static final boolean isIsolated(int uid) {
        int appId = UserHandle.getAppId(uid);
        return (appId >= FIRST_ISOLATED_UID && appId <= LAST_ISOLATED_UID)
                || (appId >= FIRST_APP_ZYGOTE_ISOLATED_UID && appId <= LAST_APP_ZYGOTE_ISOLATED_UID);
    }

    /** Whether the uid is an SDK sandbox's, as the tag's tells it, which Environment asks. Minato's app never is. */
    public static boolean isSdkSandboxUid(int uid) {
        uid = UserHandle.getAppId(uid);
        return uid >= FIRST_SDK_SANDBOX_UID && uid <= LAST_SDK_SANDBOX_UID;
    }

    /**
     * Android's sets the calling thread's nice value (android_util_Process.cpp). Minato leaves its threads at the
     * priority iOS gives them, so HandlerThread's call changes nothing.
     */
    public static final void setThreadPriority(int priority) {
    }

    /** Sends SIGKILL to the process, as the tag's sendSignal does, with its log line. */
    public static final void killProcess(int pid) {
        if (pid > 0) {
            Log.i("Process", "Sending signal. PID: " + pid + " SIG: " + OsConstants.SIGKILL);
            try {
                Os.kill(pid, OsConstants.SIGKILL);
            } catch (ErrnoException e) {
                // As the tag's: kill's result is not checked.
            }
        }
    }

    /** Whether the VM runs 64-bit code, as the tag's Process asks VMRuntime. */
    public static final boolean is64Bit() {
        return VMRuntime.getRuntime().is64Bit();
    }

    /** As AOSP's: no uid; BroadcastReceiver's pending result starts with it. */
    public static final int INVALID_UID = -1;

    /** As AOSP's: the priority of a thread that shows the display's frames (Instrumentation's helper threads). */
    public static final int THREAD_PRIORITY_URGENT_DISPLAY = -8;

    /** Whether this is an SDK sandbox's process, whose activities Instrumentation checks: never on Minato. */
    public static boolean isSdkSandbox() {
        return false;
    }

    // When the process started, as the tag's keeps it: ActivityThread's bind sets them, as handleBindApplication does.
    private static long sStartElapsedRealtime;
    private static long sStartUptimeMillis;
    private static long sStartRequestedElapsedRealtime;
    private static long sStartRequestedUptimeMillis;

    /** As AOSP's: the elapsedRealtime at which the process started, before the app's code ran. */
    public static long getStartElapsedRealtime() {
        return sStartElapsedRealtime;
    }

    /** As AOSP's: the uptimeMillis at which the process started, before the app's code ran. */
    public static long getStartUptimeMillis() {
        return sStartUptimeMillis;
    }

    /** As AOSP's: the elapsedRealtime at which the system was about to start the process. */
    public static long getStartRequestedElapsedRealtime() {
        return sStartRequestedElapsedRealtime;
    }

    /** As AOSP's: the uptimeMillis at which the system was about to start the process. */
    public static long getStartRequestedUptimeMillis() {
        return sStartRequestedUptimeMillis;
    }

    /** @hide ActivityThread's at the bind, as AOSP's handleBindApplication calls it. */
    public static final void setStartTimes(long elapsedRealtime, long uptimeMillis,
            long startRequestedElapsedRealtime, long startRequestedUptime) {
        sStartElapsedRealtime = elapsedRealtime;
        sStartUptimeMillis = uptimeMillis;
        sStartRequestedElapsedRealtime = startRequestedElapsedRealtime;
        sStartRequestedUptimeMillis = startRequestedUptime;
    }
}
