package io.github.kisame76.minato.test.vsync;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Choreographer;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * ADR-0041 a)'s test of Java's Choreographer (decision 6): AOSP's class, whose DisplayEventReceiver's natives Minato
 * gives on libandroid's AChoreographer. It starts in Application.onCreate on the main thread, as androidx's
 * profileinstaller asks for its frame callback while the providers install, and goes on step by step in the main
 * Looper's callbacks:
 * - getInstance gives one instance on the main thread;
 * - three frame callbacks in a row: rising frame times, each at or before its callback's System.nanoTime and less
 *   than 100 ms before it;
 * - postFrameCallbackDelayed by 100 ms, posted once the launcher activity has drawn, so that the start's work no
 *   longer holds the main thread, runs 100 ms or more after its post's SystemClock.uptimeMillis;
 * - a removed frame callback never runs, and one posted beside it does;
 * - postVsyncCallback's FrameData: at least one frame timeline, the preferred one among them, each deadline before
 *   its expected presentation, both rising from one timeline to the next;
 * - after the main thread was blocked 60 ms while a vsync was due, the next FrameData's preferred deadline lies ahead
 *   of System.nanoTime, as FrameData.update asks the receiver for the latest timelines then;
 * - on a HandlerThread, getInstance gives another instance, whose frame callback runs on that thread.
 * It logs with the tag vsync-test and ends with "done"; the lines that differ by device, the frames' spacing and the
 * timelines' count and spacing, go to vsync-test-device. tools/android-reference.sh compares it with Android 15's
 * log.
 */
public class VsyncApplication extends Application {
    private static final String TAG = "vsync-test";
    private static final long MS = 1_000_000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private Choreographer choreographer;
    // The delayed callback's post waits for both.
    private boolean threeFramesDone, firstDrawDone;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            choreographer = Choreographer.getInstance();
            Log.i(TAG, "getInstance on the main thread: the same twice " + (choreographer == Choreographer.getInstance()));
            afterFirstDraw();
            threeFrames(new long[3], new long[3], 0);
        } catch (Throwable t) {
            failed(t);
        }
    }

    private static void failed(Throwable t) {
        Log.i(TAG, "failed: " + t);
        // Its stack, which differs by device, is only shown.
        Log.i(TAG + "-device", Log.getStackTraceString(t));
        Log.i(TAG, "done");
    }

    /** Frame callbacks in a row: each posts the next one. */
    private void threeFrames(long[] frameTimes, long[] calledAt, int index) {
        choreographer.postFrameCallback(frameTimeNanos -> {
            calledAt[index] = System.nanoTime();
            frameTimes[index] = frameTimeNanos;
            if (index < 2) {
                threeFrames(frameTimes, calledAt, index + 1);
                return;
            }
            boolean rising = frameTimes[0] < frameTimes[1] && frameTimes[1] < frameTimes[2];
            boolean before = true, recent = true;
            for (int frame = 0; frame < 3; frame++) {
                before &= frameTimes[frame] <= calledAt[frame];
                recent &= calledAt[frame] - frameTimes[frame] < 100 * MS;
            }
            Log.i(TAG, "3 frame callbacks: frame times rising " + rising + ", each at or before its callback "
                    + before + ", less than 100 ms before it " + recent);
            Log.i(TAG + "-device", "frames " + (frameTimes[1] - frameTimes[0]) / 1000 + " and "
                    + (frameTimes[2] - frameTimes[1]) / 1000 + " us apart");
            threeFramesDone = true;
            delayedWhenReady();
        });
    }

    /** Notes the launcher activity's first draw in a message after it, as onDraw may not remove its listener. */
    private void afterFirstDraw() {
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            @Override
            public void onActivityCreated(Activity activity, Bundle savedInstanceState) {
                unregisterActivityLifecycleCallbacks(this);
                final View decor = activity.getWindow().getDecorView();
                decor.getViewTreeObserver().addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
                    @Override
                    public void onDraw() {
                        final ViewTreeObserver.OnDrawListener listener = this;
                        main.post(() -> {
                            if (firstDrawDone) {
                                return;
                            }
                            decor.getViewTreeObserver().removeOnDrawListener(listener);
                            firstDrawDone = true;
                            delayedWhenReady();
                        });
                    }
                });
            }

            @Override
            public void onActivityStarted(Activity activity) {
            }

            @Override
            public void onActivityResumed(Activity activity) {
            }

            @Override
            public void onActivityPaused(Activity activity) {
            }

            @Override
            public void onActivityStopped(Activity activity) {
            }

            @Override
            public void onActivitySaveInstanceState(Activity activity, Bundle outState) {
            }

            @Override
            public void onActivityDestroyed(Activity activity) {
            }
        });
    }

    private void delayedWhenReady() {
        if (threeFramesDone && firstDrawDone) {
            delayed();
        }
    }

    private void delayed() {
        // In whole ms, as Choreographer counts the delay from uptimeMillis (doCallbacks): 12 frames at 120 Hz are
        // 99.999996 ms, so the callback may run just under 100 ms after a System.nanoTime() taken here.
        final long posted = SystemClock.uptimeMillis() * MS;
        choreographer.postFrameCallbackDelayed(frameTimeNanos -> {
            Log.i(TAG, "postFrameCallbackDelayed(100): ran 100 ms or more after its post "
                    + (System.nanoTime() - posted >= 100 * MS));
            removed();
        }, 100);
    }

    private void removed() {
        final boolean[] ran = new boolean[1];
        Choreographer.FrameCallback removed = frameTimeNanos -> ran[0] = true;
        choreographer.postFrameCallback(removed);
        choreographer.postFrameCallback(frameTimeNanos -> {
            // A frame later, the removed callback has had its chance.
            choreographer.postFrameCallback(later -> {
                Log.i(TAG, "removeFrameCallback: the removed callback ran " + ran[0] + ", the one beside it ran true");
                vsync();
            });
        });
        choreographer.removeFrameCallback(removed);
    }

    private void vsync() {
        choreographer.postVsyncCallback(data -> {
            Choreographer.FrameTimeline[] timelines = data.getFrameTimelines();
            Choreographer.FrameTimeline preferred = data.getPreferredFrameTimeline();
            boolean among = false, ordered = true, rising = true;
            for (int index = 0; index < timelines.length; index++) {
                among |= timelines[index] == preferred;
                ordered &= timelines[index].getDeadlineNanos() < timelines[index].getExpectedPresentationTimeNanos();
                if (index > 0) {
                    rising &= timelines[index].getDeadlineNanos() > timelines[index - 1].getDeadlineNanos()
                            && timelines[index].getExpectedPresentationTimeNanos()
                                    > timelines[index - 1].getExpectedPresentationTimeNanos();
                }
            }
            Log.i(TAG, "postVsyncCallback: timelines " + (timelines.length >= 1) + ", the preferred among them "
                    + among + ", each deadline before its presentation " + ordered + ", both rising " + rising
                    + ", the frame time at or before the preferred deadline "
                    + (data.getFrameTimeNanos() <= preferred.getDeadlineNanos()));
            long spacing = timelines.length > 1
                    ? timelines[1].getExpectedPresentationTimeNanos() - timelines[0].getExpectedPresentationTimeNanos()
                    : 0;
            Log.i(TAG + "-device", timelines.length + " timelines, " + spacing / 1000 + " us apart");
            blocked();
        });
    }

    /** A vsync comes due while the main thread sleeps in a message of its own. */
    private void blocked() {
        choreographer.postVsyncCallback(data -> {
            Log.i(TAG, "after the main thread was blocked 60 ms, the preferred deadline lies ahead "
                    + (data.getPreferredFrameTimeline().getDeadlineNanos() > System.nanoTime()));
            otherThread();
        });
        main.post(() -> SystemClock.sleep(60));
    }

    private void otherThread() {
        HandlerThread thread = new HandlerThread("vsync-test-thread");
        thread.start();
        new Handler(thread.getLooper()).post(() -> {
            Choreographer own = Choreographer.getInstance();
            own.postFrameCallback(frameTimeNanos -> {
                Log.i(TAG, "on a HandlerThread: another instance " + (own != choreographer) + ", its callback on "
                        + Thread.currentThread().getName());
                thread.quitSafely();
                main.post(() -> Log.i(TAG, "done"));
            });
        });
    }
}
