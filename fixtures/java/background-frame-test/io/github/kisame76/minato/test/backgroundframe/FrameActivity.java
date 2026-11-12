package io.github.kisame76.minato.test.backgroundframe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Choreographer;
import android.view.View;

/**
 * Frames at the background: a Choreographer.postFrameCallback loop runs from the start; at onPause the activity asks
 * its view for a layout, so ViewRootImpl schedules a traversal (and its sync barrier on the main looper) just as the
 * app goes to the background. On Android the requested vsync comes for a stopped app too (SurfaceFlinger's
 * EventThread has no gate for it), the traversal runs and onStop follows onPause at once. The test logs whether
 * onStop came within a second of onPause and whether the frame loop ticked in the first second after onStop while the
 * app stayed stopped; after the return it waits for frames again, says "done" and finishes. Messages of its own go
 * through an asynchronous handler, which a sync barrier does not hold back.
 */
public class FrameActivity extends Activity {
    static final String TAG = "background-frame-test";
    private final Handler handler = Handler.createAsync(Looper.getMainLooper());
    private Choreographer choreographer;
    private View view;
    private long ticks;
    private long pausedAt;
    private boolean stopped;
    private boolean returned;
    private boolean readySaid;
    private long resumedTicks = -1;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private final Choreographer.FrameCallback loop = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            ticks++;
            if (resumedTicks >= 0 && ticks - resumedTicks >= 3) {
                resumedTicks = -1;
                if (!readySaid) {
                    readySaid = true;
                    log("ready");
                } else if (returned) {
                    log("after the return: frames again");
                    log("done");
                    finish();
                    return;
                }
            }
            choreographer.postFrameCallback(this);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        view = new View(this);
        view.setBackgroundColor(0xff2060a0);
        setContentView(view);
        choreographer = Choreographer.getInstance();
        choreographer.postFrameCallback(loop);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        stopped = false;
        returned = true;
        log("onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        log("onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        log("onResume");
        resumedTicks = ticks;
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (isFinishing()) return;  // the end after "done": nothing more to log
        log("onPause");
        pausedAt = SystemClock.uptimeMillis();
        view.requestLayout();
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (isFinishing()) return;
        long after = SystemClock.uptimeMillis() - pausedAt;
        log("onStop, within a second of onPause: " + (after < 1000));
        stopped = true;
        final long atStop = ticks;
        handler.postDelayed(() -> {
            if (stopped) {
                log("the frame loop in the first second after onStop: " + (ticks > atStop ? "ticks" : "no ticks"));
            } else {
                log("the frame loop in the first second after onStop: back before the second ended");
            }
        }, 1000);
    }
}
