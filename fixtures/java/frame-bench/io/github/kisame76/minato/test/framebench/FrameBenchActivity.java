package io.github.kisame76.minato.test.framebench;

import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.Choreographer;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * M6's baseline of the Java apps' frame times: two phases of FRAMES frames each (after WARMUP): a view tree whose texts
 * change and which lays out again every frame, and the same tree under animators (translation, alpha and a value
 * animator that sets a text). Each frame's time is Choreographer's frame time to its commit callback, which runs after
 * the traversal; the p50, p99 and the largest of each phase are logged under the "-device" tag, which is only shown.
 */
public class FrameBenchActivity extends Activity {
    private static final String TAG = "frame-bench-device";
    private static final int WARMUP = 20;
    private static final int FRAMES = 240;
    private static final int ROWS = 12;
    private static final int COLUMNS = 4;

    private final List<TextView> mTexts = new ArrayList<>();
    private final List<Double> mTimes = new ArrayList<>();
    private LinearLayout mRoot;
    private int mPhase;
    private int mFrame;
    private boolean mStarted;
    private final List<ObjectAnimator> mAnimators = new ArrayList<>();
    private ValueAnimator mValue;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mRoot = new LinearLayout(this);
        mRoot.setOrientation(LinearLayout.VERTICAL);
        for (int r = 0; r < ROWS; r++) {
            final LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            for (int c = 0; c < COLUMNS; c++) {
                final TextView view = (r + c) % 5 == 0 ? new Button(this) : new TextView(this);
                view.setText("cell " + r + "." + c);
                view.setPadding(8, 8, 8, 8);
                row.addView(view, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
                mTexts.add(view);
            }
            mRoot.addView(row, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));
        }
        setContentView(mRoot);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            beginPhase(0);
        }
    }

    private void beginPhase(int phase) {
        mPhase = phase;
        mFrame = 0;
        mTimes.clear();
        if (phase == 1) {
            for (int i = 0; i < mTexts.size(); i += 2) {
                final ObjectAnimator move = ObjectAnimator.ofFloat(mTexts.get(i), "translationX", 0f, 20f);
                move.setDuration(1000);
                move.setRepeatCount(ValueAnimator.INFINITE);
                move.setRepeatMode(ValueAnimator.REVERSE);
                move.start();
                mAnimators.add(move);
                final ObjectAnimator fade = ObjectAnimator.ofFloat(mTexts.get(i), "alpha", 1f, 0.4f);
                fade.setDuration(700);
                fade.setRepeatCount(ValueAnimator.INFINITE);
                fade.setRepeatMode(ValueAnimator.REVERSE);
                fade.start();
                mAnimators.add(fade);
            }
            mValue = ValueAnimator.ofInt(0, 1000);
            mValue.setDuration(1000);
            mValue.setRepeatCount(ValueAnimator.INFINITE);
            mValue.addUpdateListener(a -> mTexts.get(1).setText("value " + a.getAnimatedValue()));
            mValue.start();
        }
        schedule();
    }

    private void schedule() {
        final Choreographer choreographer = Choreographer.getInstance();
        choreographer.postCallback(Choreographer.CALLBACK_ANIMATION, this::change, null);
        choreographer.postCallback(Choreographer.CALLBACK_COMMIT, this::committed, null);
    }

    /** Phase 0 changes every text, which lays the tree out again; phase 1 leaves that to the animators. */
    private void change() {
        if (mPhase == 0) {
            for (int i = 0; i < mTexts.size(); i++) {
                mTexts.get(i).setText("cell " + mFrame + "." + i);
            }
        }
    }

    private void committed() {
        final double ms = (System.nanoTime() - Choreographer.getInstance().getFrameTimeNanos()) / 1e6;
        if (mFrame++ >= WARMUP) {
            mTimes.add(ms);
        }
        if (mFrame < WARMUP + FRAMES) {
            schedule();
            return;
        }
        final double[] sorted = mTimes.stream().mapToDouble(Double::doubleValue).toArray();
        Arrays.sort(sorted);
        Log.i(TAG, String.format("phase %d: frames %d, p50 %.2f ms, p99 %.2f ms, max %.2f ms", mPhase, sorted.length,
                sorted[sorted.length / 2], sorted[(int) Math.floor(sorted.length * 0.99)], sorted[sorted.length - 1]));
        if (mPhase == 0) {
            beginPhase(1);
            return;
        }
        for (ObjectAnimator animator : mAnimators) {
            animator.cancel();
        }
        mValue.cancel();
        Log.i("frame-bench", "done");
    }
}
