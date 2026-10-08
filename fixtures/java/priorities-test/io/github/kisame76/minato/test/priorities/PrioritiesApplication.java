package io.github.kisame76.minato.test.priorities;

import android.app.Application;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.util.Log;

import java.util.concurrent.CountDownLatch;

/**
 * A thread's priority as an app sees it: Process.setThreadPriority and getThreadPriority for the caller, another
 * thread and an unknown one, a HandlerThread's priority, and the nice values that ART gives a Java thread's priority
 * at Thread.setPriority and at its start. Everything runs on a thread of the test's own, as the main thread's
 * priority is the system's to raise while the app is on top.
 */
public class PrioritiesApplication extends Application {
    static final String TAG = "priorities-test";
    // No thread has this ID: Linux's pid_max is at most 4,194,304.
    static final int UNKNOWN = Integer.MAX_VALUE;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Call {
        void run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            call.run();
            return "fine";
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Thread test = new Thread(this::run, "priorities");
        test.start();
        try {
            test.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        log("done");
    }

    private void run() {
        log("a new thread: " + Process.getThreadPriority(0) + ", by its tid "
                + Process.getThreadPriority(Process.myTid()));
        Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        log("THREAD_PRIORITY_BACKGROUND: " + Process.getThreadPriority(0));
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        log("THREAD_PRIORITY_URGENT_AUDIO: " + Process.getThreadPriority(0));
        Process.setThreadPriority(25);
        log("25: " + Process.getThreadPriority(0));
        Process.setThreadPriority(-25);
        log("-25: " + Process.getThreadPriority(0));
        Process.setThreadPriority(Process.myTid(), Process.THREAD_PRIORITY_DEFAULT);
        log("by its tid, THREAD_PRIORITY_DEFAULT: " + Process.getThreadPriority(0));
        anotherThread();
        log("an unknown thread's priority: " + outcome(() -> Process.getThreadPriority(UNKNOWN)));
        log("an unknown thread set to 0: " + outcome(() -> Process.setThreadPriority(UNKNOWN, 0)));
        // libutils' androidSetThreadPriority leaves a thread alone whose getpriority already gives the value: for an
        // unknown thread getpriority's -1.
        log("an unknown thread set to -1: " + outcome(() -> Process.setThreadPriority(UNKNOWN, -1)));
        handlerThread();
        javaPriorities();
    }

    private void anotherThread() {
        final int[] tid = new int[1];
        final CountDownLatch ready = new CountDownLatch(1);
        final CountDownLatch set = new CountDownLatch(1);
        final int[] seen = new int[1];
        Thread other = new Thread(() -> {
            tid[0] = Process.myTid();
            ready.countDown();
            awaitQuietly(set);
            seen[0] = Process.getThreadPriority(0);
        }, "other");
        other.start();
        awaitQuietly(ready);
        Process.setThreadPriority(tid[0], 5);
        log("another thread, set to 5 from here: " + Process.getThreadPriority(tid[0]));
        set.countDown();
        joinQuietly(other);
        log("and as it sees itself: " + seen[0]);
    }

    private void handlerThread() {
        HandlerThread thread = new HandlerThread("background", Process.THREAD_PRIORITY_BACKGROUND);
        thread.start();
        final int[] seen = new int[1];
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(thread.getLooper()).post(() -> {
            seen[0] = Process.getThreadPriority(0);
            done.countDown();
        });
        awaitQuietly(done);
        log("a HandlerThread at THREAD_PRIORITY_BACKGROUND: " + seen[0] + ", from outside "
                + Process.getThreadPriority(thread.getThreadId()));
        thread.quitSafely();
        joinQuietly(thread);
    }

    // ART sets a Java thread's nice from its Java priority at Thread.setPriority and when the thread starts.
    private void javaPriorities() {
        Thread self = Thread.currentThread();
        self.setPriority(Thread.MAX_PRIORITY);
        log("Thread.setPriority(MAX_PRIORITY): " + Process.getThreadPriority(0));
        self.setPriority(Thread.MIN_PRIORITY);
        log("Thread.setPriority(MIN_PRIORITY): " + Process.getThreadPriority(0));
        self.setPriority(Thread.NORM_PRIORITY);
        log("Thread.setPriority(NORM_PRIORITY): " + Process.getThreadPriority(0));
        Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        log("a thread started by one at " + Process.getThreadPriority(0) + ": " + startedAt(Thread.NORM_PRIORITY));
        Process.setThreadPriority(Process.THREAD_PRIORITY_DEFAULT);
        log("a thread started at Java priority 1: " + startedAt(Thread.MIN_PRIORITY));
        log("a thread started at Java priority 7: " + startedAt(7));
        log("this thread's Java priority, after Process.setThreadPriority: " + self.getPriority());
    }

    private static int startedAt(int javaPriority) {
        final int[] seen = new int[1];
        Thread thread = new Thread(() -> seen[0] = Process.getThreadPriority(0), "started");
        thread.setPriority(javaPriority);
        thread.start();
        joinQuietly(thread);
        return seen[0];
    }

    private static void awaitQuietly(CountDownLatch latch) {
        try {
            latch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static void joinQuietly(Thread thread) {
        try {
            thread.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
