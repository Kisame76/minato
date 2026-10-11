package io.github.kisame76.minato.test.mainlooper;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The main thread's Looper under posts from another thread (the Unity test APK's retests: Runnables that engines and SDKs post to
 * the main Looper ran seconds late): once the launcher activity runs, a worker posts to the main Looper and measures
 * how long each Runnable waits, single posts after the main thread went idle, one after a longer idle, a delayed one
 * and a burst. Each wait is held against a bound well above Android 15's, so that the log is the same on the
 * emulator and on Minato; the waits themselves are only shown.
 */
public class MainLooperApplication extends Application {
    private static final String TAG = "main-looper-test";
    /** Far above the emulator's waits (well under 10 ms), far below the seconds the Unity test APK saw. */
    private static final long BOUND_MS = 250;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Long> waits = new ArrayList<>();

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** The waits, which differ by device, are only shown. */
    private static void logDevice(String line) {
        Log.i(TAG + "-device", line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Thread worker = new Thread(this::run, "main-looper-worker");
        worker.start();
    }

    /** Posts a Runnable to the main Looper and waits for it: how long it waited there, or -1 after 10 s. */
    private long postAndWait() throws InterruptedException {
        CountDownLatch ran = new CountDownLatch(1);
        long[] at = new long[1];
        long posted = SystemClock.uptimeMillis();
        main.post(() -> {
            at[0] = SystemClock.uptimeMillis();
            ran.countDown();
        });
        if (!ran.await(10, TimeUnit.SECONDS)) {
            return -1;
        }
        return at[0] - posted;
    }

    private void run() {
        try {
            // The activity starts and the main thread goes back to its Looper meanwhile.
            SystemClock.sleep(1000);

            int ran = 0;
            boolean bounded = true;
            for (int i = 0; i < 5; i++) {
                long wait = postAndWait();
                waits.add(wait);
                if (wait >= 0) {
                    ran++;
                }
                bounded &= wait >= 0 && wait < BOUND_MS;
                SystemClock.sleep(100);
            }
            log("single posts ran " + ran + " of 5, each within " + BOUND_MS + " ms " + bounded);

            // A longer idle, in which the main thread may poll with a long timeout.
            SystemClock.sleep(2000);
            long afterIdle = postAndWait();
            waits.add(afterIdle);
            log("a post after 2 s idle ran " + (afterIdle >= 0) + ", within " + BOUND_MS + " ms "
                    + (afterIdle >= 0 && afterIdle < BOUND_MS));

            CountDownLatch delayed = new CountDownLatch(1);
            long[] delayedAt = new long[1];
            long start = SystemClock.uptimeMillis();
            main.postDelayed(() -> {
                delayedAt[0] = SystemClock.uptimeMillis();
                delayed.countDown();
            }, 300);
            boolean delayedRan = delayed.await(10, TimeUnit.SECONDS);
            long delayedWait = delayedAt[0] - start;
            waits.add(delayedWait - 300);
            log("a post delayed by 300 ms ran " + delayedRan + ", not early " + (delayedWait >= 300)
                    + ", within " + BOUND_MS + " ms of its time " + (delayedRan && delayedWait < 300 + BOUND_MS));

            int count = 20;
            CountDownLatch burst = new CountDownLatch(count);
            List<Integer> order = new ArrayList<>();
            long burstStart = SystemClock.uptimeMillis();
            long[] burstEnd = new long[1];
            for (int i = 0; i < count; i++) {
                final int index = i;
                main.post(() -> {
                    order.add(index);
                    burstEnd[0] = SystemClock.uptimeMillis();
                    burst.countDown();
                });
            }
            boolean burstRan = burst.await(10, TimeUnit.SECONDS);
            boolean inOrder = true;
            for (int i = 0; i < order.size(); i++) {
                inOrder &= order.get(i) == i;
            }
            long burstWait = burstEnd[0] - burstStart;
            waits.add(burstWait);
            log("a burst of " + count + " posts ran " + burstRan + ", in order " + inOrder + ", within " + BOUND_MS
                    + " ms " + (burstRan && burstWait < BOUND_MS));
            logDevice("waits in ms: " + waits);
        } catch (Throwable t) {
            log("failed: " + t);
        }
        log("done");
    }
}
