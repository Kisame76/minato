/*
 * Our InsetsController, under AOSP's name as ViewRootImpl is (ADR-0041 decisions 2 and 8; ADR-0042 b's start stops):
 * the window's WindowInsetsController, which ViewRootImpl makes and into which setView replays what the app asked of
 * DecorView's PendingInsetsController before the window was attached. AOSP's (2,300 lines) runs the window manager's
 * insets controls with animations and leashes; ours keeps what the app asks and answers it as AOSP's does
 * (InsetsController.java:2074-2142, 2149-2192 at the tag):
 *   - the requested visible types, all but the IME's until the app shows or hides some;
 *   - the appearance the app sets, over the one its theme gives, and the behavior once the app sets one;
 *   - no insets control: the window manager gives Minato's window none yet, so no type is controllable and an
 *     animation's control is cancelled at once.
 * So hiding the bars, as immersive apps do, changes no insets yet: decision 8's insets and ADR-0038's status bar come
 * later.
 */
package android.view;

import android.os.CancellationSignal;
import android.view.animation.Interpolator;

import java.util.ArrayList;
import java.util.Objects;

/** @hide */
public class InsetsController implements WindowInsetsController {
    private final InsetsState mState = new InsetsState();
    private final ArrayList<OnControllableInsetsChangedListener> mControllableInsetsChangedListeners =
            new ArrayList<>();
    private int mRequestedVisibleTypes = WindowInsets.Type.defaultVisible();
    /** The window's appearance flags, as AOSP's host keeps them in the window's attributes. */
    private int mAppearance;
    private int mAppearanceControlled;
    private int mAppearanceFromResource;
    private int mBehavior = BEHAVIOR_DEFAULT;
    private boolean mBehaviorControlled;
    private boolean mAnimationsDisabled;

    @Override
    public void show(int types) {
        mRequestedVisibleTypes |= types;
    }

    @Override
    public void hide(int types) {
        mRequestedVisibleTypes &= ~types;
    }

    /** As AOSP's for types it has no control of: the listener hears at once that it gets none. */
    @Override
    public void controlWindowInsetsAnimation(int types, long durationMillis, Interpolator interpolator,
            CancellationSignal cancellationSignal, WindowInsetsAnimationControlListener listener) {
        listener.onCancelled(null);
    }

    /** Nothing animates, so nothing is logged. */
    @Override
    public void setSystemDrivenInsetsAnimationLoggingListener(WindowInsetsAnimationControlListener listener) {
    }

    @Override
    public void setSystemBarsAppearance(int appearance, int mask) {
        mAppearanceControlled |= mask;
        mAppearance = (mAppearance & ~mask) | (appearance & mask);
    }

    /** As AOSP's: the theme's flags, which do not change those the app set. */
    @Override
    public void setSystemBarsAppearanceFromResource(int appearance, int mask) {
        mAppearanceFromResource = (mAppearanceFromResource & ~mask) | (appearance & mask);
        final int fromResource = mask & ~mAppearanceControlled;
        mAppearance = (mAppearance & ~fromResource) | (appearance & fromResource);
    }

    /** As AOSP's: the requested appearance, not the implied one. */
    @Override
    public int getSystemBarsAppearance() {
        return (mAppearance & mAppearanceControlled) | (mAppearanceFromResource & ~mAppearanceControlled);
    }

    @Override
    public void setSystemBarsBehavior(int behavior) {
        mBehaviorControlled = true;
        mBehavior = behavior;
    }

    /** As AOSP's: the requested behavior, BEHAVIOR_DEFAULT until the app sets one. */
    @Override
    public int getSystemBarsBehavior() {
        return mBehaviorControlled ? mBehavior : BEHAVIOR_DEFAULT;
    }

    @Override
    public void setAnimationsDisabled(boolean disable) {
        mAnimationsDisabled = disable;
    }

    @Override
    public InsetsState getState() {
        return mState;
    }

    @Override
    public int getRequestedVisibleTypes() {
        return mRequestedVisibleTypes;
    }

    /** As AOSP's: the listener hears the controllable types at once, none on Minato. */
    @Override
    public void addOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener) {
        Objects.requireNonNull(listener);
        mControllableInsetsChangedListeners.add(listener);
        listener.onControllableInsetsChanged(this, 0);
    }

    @Override
    public void removeOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener) {
        Objects.requireNonNull(listener);
        mControllableInsetsChangedListeners.remove(listener);
    }
}
