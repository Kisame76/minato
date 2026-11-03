package io.github.kisame76.minato.test.startstops;

import android.app.Activity;
import android.app.Fragment;
import android.os.Bundle;
import android.transition.AutoTransition;
import android.transition.Fade;
import android.transition.Transition;
import android.util.Log;
import android.util.Rational;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * ADR-0042 b's test of Godot's first three start stops past 7a, as Godot's start
 * meets them:
 * <ul>
 *   <li>two Rationals for picture-in-picture bounds, made as GodotActivity's fields are;
 *   <li>the window's insets controller in onCreate, before the window is attached, as androidx's EdgeToEdge uses it:
 *       the decor view first, then an appearance and a behavior set and read back, and the system bars hidden;
 *   <li>a platform fragment added and found by its tag, as androidx's ReportFragment is on every ComponentActivity;
 *   <li>an AutoTransition's ordering and children;
 *   <li>at the window's first focus, after the attach: the controller again, with what it replayed.
 * </ul>
 */
public class StartStopsActivity extends Activity {
    private static final String TAG = "start-stops-test";

    private interface Section {
        void run() throws Exception;
    }

    private final Rational mMinRatio = new Rational(100, 239);
    private final Rational mMaxRatio = new Rational(239, 100);
    private boolean mFocused;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        guarded("rationals", this::rationals);
        guarded("insets before attach", this::insetsBeforeAttach);
        guarded("platform fragment", this::platformFragment);
        guarded("auto transition", this::autoTransition);
    }

    private void rationals() {
        Log.i(TAG, "rationals: " + mMinRatio + " = " + mMinRatio.floatValue() + ", " + mMaxRatio + " = "
                + mMaxRatio.floatValue() + ", compared " + mMinRatio.compareTo(mMaxRatio));
    }

    private void insetsBeforeAttach() {
        getWindow().getDecorView();
        WindowInsetsController controller = getWindow().getInsetsController();
        Log.i(TAG, "before attach: controller " + name(controller));
        controller.setSystemBarsAppearance(WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS,
                WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS);
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        controller.hide(WindowInsets.Type.systemBars());
        Log.i(TAG, "before attach: " + state(controller));
    }

    @SuppressWarnings("deprecation") // The platform fragments, as androidx's ReportFragment uses them.
    private void platformFragment() {
        getFragmentManager().beginTransaction().add(new Fragment(), TAG).commit();
        getFragmentManager().executePendingTransactions();
        Fragment found = getFragmentManager().findFragmentByTag(TAG);
        Log.i(TAG, "platform fragment found by its tag: " + (found != null) + ", added "
                + (found != null && found.isAdded()));
    }

    private void autoTransition() {
        AutoTransition transition = new AutoTransition();
        StringBuilder children = new StringBuilder();
        for (int i = 0; i < transition.getTransitionCount(); i++) {
            Transition child = transition.getTransitionAt(i);
            children.append(i == 0 ? "" : ", ").append(child.getClass().getSimpleName());
            if (child instanceof Fade) {
                children.append(" mode ").append(((Fade) child).getMode());
            }
        }
        Log.i(TAG, "auto transition: ordering " + transition.getOrdering() + ", children " + children);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mFocused) {
            return;
        }
        mFocused = true;
        guarded("insets after attach", () -> {
            WindowInsetsController controller = getWindow().getInsetsController();
            Log.i(TAG, "after attach: controller " + name(controller) + ", " + state(controller));
        });
        Log.i(TAG, "done");
    }

    private static String state(WindowInsetsController controller) {
        return "appearance 0x" + Integer.toHexString(controller.getSystemBarsAppearance()) + ", behavior "
                + controller.getSystemBarsBehavior();
    }

    private static String name(Object object) {
        return object == null ? "null" : object.getClass().getName();
    }

    /** Runs a section; what it throws is logged with its name. */
    private static void guarded(String name, Section section) {
        try {
            section.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e);
        }
    }
}
