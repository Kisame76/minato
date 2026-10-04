package io.github.kisame76.minato.test.animator;

import android.animation.Animator;
import android.animation.AnimatorInflater;
import android.animation.AnimatorListenerAdapter;
import android.animation.AnimatorSet;
import android.animation.ArgbEvaluator;
import android.animation.Keyframe;
import android.animation.LayoutTransition;
import android.animation.ObjectAnimator;
import android.animation.PropertyValuesHolder;
import android.animation.StateListAnimator;
import android.animation.TimeAnimator;
import android.animation.TimeInterpolator;
import android.animation.ValueAnimator;
import android.app.Application;
import android.os.SystemClock;
import android.util.AndroidRuntimeException;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.view.animation.AccelerateInterpolator;
import android.view.animation.AnimationUtils;
import android.view.animation.AnticipateInterpolator;
import android.view.animation.AnticipateOvershootInterpolator;
import android.view.animation.BounceInterpolator;
import android.view.animation.CycleInterpolator;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.LinearInterpolator;
import android.view.animation.OvershootInterpolator;
import android.view.animation.PathInterpolator;
import java.util.Locale;

/**
 * ADR-0041 a)'s test: android.animation as an app's code uses it, in Application.onCreate, as there is no activity
 * before step c2c3. It logs, with the tag animator-test, first what needs no frame: the interpolators' and an
 * evaluator's values, animators set to play times, setters that animators find by name (PropertyValuesHolder's
 * natives), animators inflated from XML, and a layout's LayoutTransition and state list animator. Then, on
 * Choreographer's frames: an AnimatorSet's callbacks in order with their end values, a TimeAnimator's first tick,
 * View.animate()'s end action, and an animator started off a looper thread; "done" ends it. The Android emulator
 * ticks at 60 Hz and Minato's clock at up to 120 Hz, so nothing logged counts frames or times beyond a lower bound.
 * tools/android-reference.sh compares the log with Android 15's.
 */
public class AnimatorApplication extends Application {
    private static final String TAG = "animator-test";
    private static final float[] FRACTIONS = {0f, 0.25f, 0.5f, 0.75f, 1f};

    @Override
    public void onCreate() {
        super.onCreate();
        guarded(this::report);
    }

    // A failure ends the test at once with its stack, which differs by device and is only shown.
    private static void guarded(Runnable step) {
        try {
            step.run();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
            Log.i(TAG, "done");
        }
    }

    private static String format(float value) {
        return String.format(Locale.US, "%.4f", value);
    }

    private static String name(Object object) {
        return object == null ? "null" : object.getClass().getSimpleName();
    }

    private void report() {
        Log.i(TAG, "duration scale " + ValueAnimator.getDurationScale() + ", animators enabled "
                + ValueAnimator.areAnimatorsEnabled());
        interpolators();
        Log.i(TAG, "argb halfway from black to white 0x"
                + Integer.toHexString((Integer) new ArgbEvaluator().evaluate(0.5f, 0xff000000, 0xffffffff)));
        playTimes();
        setters();
        inflated();
        onFrames(layout());
    }

    private void interpolators() {
        String[] names = {"linear", "accelerate", "accelerate 2", "decelerate", "accelerate decelerate",
            "anticipate", "overshoot", "anticipate overshoot", "bounce", "cycle 1", "path 0.4 0 0.2 1",
            "fast_out_slow_in from XML"};
        TimeInterpolator[] interpolators = {new LinearInterpolator(), new AccelerateInterpolator(),
            new AccelerateInterpolator(2f), new DecelerateInterpolator(), new AccelerateDecelerateInterpolator(),
            new AnticipateInterpolator(), new OvershootInterpolator(), new AnticipateOvershootInterpolator(),
            new BounceInterpolator(), new CycleInterpolator(1f), new PathInterpolator(0.4f, 0f, 0.2f, 1f),
            AnimationUtils.loadInterpolator(this, android.R.interpolator.fast_out_slow_in)};
        for (int index = 0; index < interpolators.length; index++) {
            StringBuilder line = new StringBuilder(names[index] + " (" + name(interpolators[index]) + "):");
            for (float fraction : FRACTIONS) {
                line.append(' ').append(format(interpolators[index].getInterpolation(fraction)));
            }
            Log.i(TAG, line.toString());
        }
    }

    // Play times and fractions set without starting: the values are computed at once, without frames.
    private void playTimes() {
        ValueAnimator animator = ValueAnimator.ofFloat(0f, 100f);
        animator.setDuration(1000);
        StringBuilder line = new StringBuilder("value animator with " + name(animator.getInterpolator()) + " at");
        for (long time : new long[] {0, 250, 500, 750, 1000}) {
            animator.setCurrentPlayTime(time);
            line.append(' ').append(time).append(" ms ").append(format((Float) animator.getAnimatedValue()));
        }
        Log.i(TAG, line.toString());

        Target target = new Target();
        ObjectAnimator keyframes = ObjectAnimator.ofPropertyValuesHolder(target, PropertyValuesHolder.ofKeyframe(
                "level", Keyframe.ofFloat(0f, 0f), Keyframe.ofFloat(0.5f, 80f), Keyframe.ofFloat(1f, 100f)));
        keyframes.setInterpolator(new LinearInterpolator());
        for (float fraction : FRACTIONS) {
            keyframes.setCurrentFraction(fraction);
        }
        Log.i(TAG, "keyframes 0 80 100 at 0, 1/4, 1/2, 3/4 and 1: " + target.calls);
    }

    private void setters() {
        Target target = new Target();
        ObjectAnimator[] animators = {ObjectAnimator.ofFloat(target, "level", 0f, 10f),
            ObjectAnimator.ofInt(target, "count", 0, 10),
            ObjectAnimator.ofMultiFloat(target, "point", new float[][] {{0f, 0f}, {10f, 20f}}),
            ObjectAnimator.ofMultiFloat(target, "rgb", new float[][] {{0f, 0f, 0f}, {1f, 0.5f, 0.25f}}),
            ObjectAnimator.ofMultiFloat(target, "box", new float[][] {{0f, 0f, 0f, 0f}, {10f, 20f, 30f, 40f}}),
            ObjectAnimator.ofMultiInt(target, "size", new int[][] {{0, 0}, {100, 200}}),
            ObjectAnimator.ofMultiInt(target, "triple", new int[][] {{0, 0, 0}, {10, 20, 30}}),
            ObjectAnimator.ofMultiInt(target, "quad", new int[][] {{0, 0, 0, 0}, {10, 20, 30, 40}}),
            ObjectAnimator.ofFloat(target, "missing", 0f, 1f)};
        for (ObjectAnimator animator : animators) {
            animator.setInterpolator(new LinearInterpolator());
            animator.setCurrentFraction(0.5f);
        }
        // "missing" has no setter: PropertyValuesHolder logs that under its own tag and calls nothing.
        Log.i(TAG, "setters by name at 1/2: " + target.calls);

        target.calls.setLength(0);
        ObjectAnimator fromGetter = ObjectAnimator.ofFloat(target, "level", 15f);
        fromGetter.setInterpolator(new LinearInterpolator());
        fromGetter.setCurrentFraction(0.5f);
        Log.i(TAG, "level from its getter to 15 at 1/2: " + target.calls);
    }

    private void inflated() {
        Log.i(TAG, "fade_in: " + describe(AnimatorInflater.loadAnimator(this, android.R.animator.fade_in)));
        int sequence = getResources().getIdentifier("sequence", "animator", getPackageName());
        Animator set = AnimatorInflater.loadAnimator(this, sequence);
        Log.i(TAG, "sequence: " + describe(set));
        tree((AnimatorSet) set, "  ");
    }

    private void tree(AnimatorSet set, String indent) {
        for (Animator child : set.getChildAnimations()) {
            Log.i(TAG, indent + describe(child));
            if (child instanceof AnimatorSet) {
                tree((AnimatorSet) child, indent + "  ");
            }
        }
    }

    // An animator's kind and timing; a value animator's values at 0, 1/2 and 1 of its overall fraction.
    private static String describe(Animator animator) {
        StringBuilder text = new StringBuilder(name(animator));
        if (animator instanceof ObjectAnimator) {
            text.append(' ').append(((ObjectAnimator) animator).getPropertyName());
        }
        text.append(", duration ").append(animator.getDuration()).append(", delay ").append(animator.getStartDelay())
                .append(", total ").append(animator.getTotalDuration()).append(", interpolator ")
                .append(name(animator.getInterpolator()));
        if (animator instanceof AnimatorSet) {
            text.append(", ").append(((AnimatorSet) animator).getChildAnimations().size()).append(" children");
        } else if (animator instanceof ValueAnimator) {
            ValueAnimator value = (ValueAnimator) animator;
            text.append(", repeat ").append(value.getRepeatCount()).append(" mode ").append(value.getRepeatMode())
                    .append(", values");
            for (float fraction : new float[] {0f, 0.5f, 1f}) {
                value.setCurrentFraction(fraction);
                Object animated = value.getAnimatedValue();
                text.append(' ').append(animated instanceof Float ? format((Float) animated) : animated);
            }
        }
        return text.toString();
    }

    private View layout() {
        int animated = getResources().getIdentifier("animated", "layout", getPackageName());
        ViewGroup root = (ViewGroup) LayoutInflater.from(this).inflate(animated, null);
        LayoutTransition transition = root.getLayoutTransition();
        Log.i(TAG, "layout transition " + name(transition));
        String[] types = {"change appearing", "change disappearing", "appearing", "disappearing", "changing"};
        for (int type = 0; type < types.length; type++) {
            Animator animator = transition.getAnimator(type);
            Log.i(TAG, "  " + types[type] + ": enabled " + transition.isTransitionTypeEnabled(type) + ", duration "
                    + transition.getDuration(type) + ", delay " + transition.getStartDelay(type) + ", stagger "
                    + transition.getStagger(type) + ", interpolator " + name(transition.getInterpolator(type))
                    + ", animator " + name(animator) + " "
                    + (animator instanceof ObjectAnimator ? ((ObjectAnimator) animator).getPropertyName() : ""));
        }

        View child = root.getChildAt(0);
        StateListAnimator stateList = child.getStateListAnimator();
        Log.i(TAG, "state list animator " + name(stateList) + ", translationZ " + child.getTranslationZ());
        // Pressing starts the matching animator on the frames; jumpToCurrentState ends it at its end value.
        child.setPressed(true);
        stateList.jumpToCurrentState();
        Log.i(TAG, "pressed: translationZ " + child.getTranslationZ());
        child.setPressed(false);
        stateList.jumpToCurrentState();
        Log.i(TAG, "released: translationZ " + child.getTranslationZ());
        return child;
    }

    private static Animator.AnimatorListener listener(String label) {
        return new AnimatorListenerAdapter() {
            @Override
            public void onAnimationStart(Animator animation) {
                Log.i(TAG, label + " start");
            }

            @Override
            public void onAnimationEnd(Animator animation) {
                Log.i(TAG, label + " end");
            }
        };
    }

    // In sequence, each step starting from the one before's end: an AnimatorSet, a TimeAnimator, View.animate().
    private void onFrames(View child) {
        long start = SystemClock.uptimeMillis();
        ValueAnimator first = ValueAnimator.ofFloat(0f, 1f).setDuration(100);
        ObjectAnimator second = ObjectAnimator.ofFloat(child, View.ALPHA, 1f, 0f).setDuration(100);
        boolean[] updated = {false};
        first.addUpdateListener(animation -> updated[0] = true);
        first.addListener(listener("first"));
        second.addListener(listener("second"));
        AnimatorSet set = new AnimatorSet();
        set.playSequentially(first, second);
        set.addListener(listener("set"));
        set.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                guarded(() -> {
                    Log.i(TAG, "set ended: first's value " + first.getAnimatedValue() + ", updated " + updated[0]
                            + ", alpha " + child.getAlpha() + ", at least 200 ms "
                            + (SystemClock.uptimeMillis() - start >= 200));
                    timeAnimator(child);
                });
            }
        });
        set.start();
    }

    private void timeAnimator(View child) {
        TimeAnimator time = new TimeAnimator();
        time.setTimeListener((animation, totalTime, deltaTime) -> {
            Log.i(TAG, "time animator's first tick: total " + totalTime + " ms, delta " + deltaTime + " ms");
            animation.end();
        });
        time.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                guarded(() -> viewAnimate(child));
            }
        });
        time.start();
    }

    // A detached view posts View.animate()'s start until it is attached, so the test starts it itself.
    private void viewAnimate(View child) {
        child.animate().alpha(1f).setDuration(100).withEndAction(() -> guarded(() -> {
            Log.i(TAG, "animate()'s end action: alpha " + child.getAlpha());
            offLooper();
        })).start();
    }

    private void offLooper() {
        Thread thread = new Thread(() -> {
            try {
                ValueAnimator.ofFloat(0f, 1f).start();
                Log.i(TAG, "off a looper: started");
            } catch (AndroidRuntimeException e) {
                Log.i(TAG, "off a looper: " + e.getClass().getName() + ": " + e.getMessage());
            }
        });
        thread.start();
        try {
            thread.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        Log.i(TAG, "done");
    }
}
