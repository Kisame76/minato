package io.github.kisame76.minato.test.vsynclag;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Choreographer;

/**
 * ADR-0041 a) decision 6's fix for a frame clock that posts late (vsync-check's flake of 2026-10-04): Java's
 * Choreographer on DisplayEventReceiver's natives under graphics-run without a session, where no frame clock runs and
 * the app's JNI library posts each tick through graphics-run's test control (MINATO_TEST_VSYNC_GRID), at the times it
 * picks, 200 ms apart:
 * - a) frame A: a tick posted after its callback, then the main thread blocked 420 ms, so that doFrame starts more
 *   than a period after the tick and Choreographer moves the frame time to the tick's grid point at or before its
 *   start, as on Android after a late frame;
 * - b) frame B, posted in A's callback; then a tick at A's frame time, as a clock posts that grid point when it posts
 *   it only after B asked for the next frame, and the grid point after it once that has passed. On Android,
 *   SurfaceFlinger's event for a request comes after it, so B runs at the second tick, not with A's frame time again.
 * Minato only: Android has no graphics-run. Each line goes to System.out with the prefix "vsync-lag-test: "; the last
 * is "done".
 */
public class VsyncLagApplication extends Application {
    private static final String PREFIX = "vsync-lag-test: ";
    private static final long MS = 1_000_000;
    private static final long PERIOD = 200 * MS;

    // Asynchronous, as Choreographer's own messages: the traversal barrier that the launcher activity's window sets
    // until its next frame would hold a plain message back, while that frame waits for the tick the message posts.
    private final Handler main = Handler.createAsync(Looper.getMainLooper());
    private Choreographer choreographer;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            System.loadLibrary("vsync-lag-test");
            if (!nativeBind()) {
                print("failed: no test control, which graphics-run gives");
                print("done");
                return;
            }
            choreographer = Choreographer.getInstance();
            frameA();
        } catch (Throwable t) {
            failed(t);
        }
    }

    private static void print(String line) {
        System.out.println(PREFIX + line);
    }

    private static void failed(Throwable t) {
        print("failed: " + t);
        print("done");
    }

    private void frameA() {
        final long[] tick = new long[1];
        choreographer.postFrameCallback(frameTime -> {
            final long calledAt = System.nanoTime();
            final boolean grid = frameTime > tick[0] && (frameTime - tick[0]) % PERIOD == 0 && frameTime <= calledAt;
            print("a) frame A, 420 ms late: its frame time on the tick's grid, at or before its start " + grid);
            try {
                choreographer.postFrameCallback(later -> frameB(frameTime, later));
                nativeTick(frameTime, PERIOD);
                final long next = frameTime + PERIOD;
                main.postDelayed(() -> nativeTick(next, PERIOD), Math.max(0, next - System.nanoTime()) / MS + 1);
            } catch (Throwable t) {
                failed(t);
            }
        });
        // After the request, as a frame clock's next tick comes.
        tick[0] = System.nanoTime();
        nativeTick(tick[0], PERIOD);
        SystemClock.sleep(420);
    }

    private static void frameB(long frameA, long frameB) {
        final long calledAt = System.nanoTime();
        print("b) frame B after a tick at A's frame time: rising " + (frameB > frameA) + ", at the next grid point "
                + (frameB == frameA + PERIOD) + ", at or before its callback " + (frameB <= calledAt));
        print("done");
    }

    /** jni/vsync_lag_test.c: whether graphics-run's test control is there. */
    private static native boolean nativeBind();

    /** jni/vsync_lag_test.c: a tick of the frame clock at {@code time} with the period {@code period}. */
    private static native void nativeTick(long time, long period);
}
