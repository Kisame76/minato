/*
 * Our DecorView, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3): the root of an activity's window, a
 * FrameLayout that holds the window's layout (screen_simple's LinearLayout with the content FrameLayout) and hands
 * the window's events to its callback, the activity, as AOSP's DecorView does: key, touch and generic events, the
 * attach and detach, and the window's focus. AOSP's (2,670 lines) also draws the system bars' backgrounds, the
 * caption and the action modes, and resizes with the window; those come with ADR-0041 a)'s insets and ADR-0038's
 * status bar, and without action modes a view's startActionMode gets none.
 */
package com.android.internal.policy;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.PendingInsetsController;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;

/** @hide */
public class DecorView extends FrameLayout {
    private final int mFeatureId;
    private PhoneWindow mWindow;
    /** The window's layout, the decor's only child. */
    ViewGroup mContentRoot;
    /** As AOSP's: what the app asks of the insets controller before the attach, which ViewRootImpl.setView replays. */
    private final PendingInsetsController mPendingInsetsController = new PendingInsetsController();

    DecorView(Context context, int featureId, PhoneWindow window, WindowManager.LayoutParams params) {
        super(context);
        mFeatureId = featureId;
        mWindow = window;
    }

    /** As AOSP's: the window's layout, inflated into the decor as its only child. */
    void onResourcesLoaded(LayoutInflater inflater, int layoutResource) {
        final View root = inflater.inflate(layoutResource, null);
        addView(root, 0, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        mContentRoot = (ViewGroup) root;
    }

    /** The window's background, which the decor draws behind everything (M4 step 10). */
    @SuppressWarnings("deprecation") // View.setBackgroundDrawable, as AOSP's setWindowBackground calls it.
    void setWindowBackground(Drawable drawable) {
        setBackgroundDrawable(drawable);
    }

    void clearContentView() {
        if (mContentRoot != null) {
            mContentRoot.removeAllViews();
        }
    }

    private Window.Callback callback() {
        final Window.Callback cb = mWindow.getCallback();
        return cb != null && !mWindow.isDestroyed() && mFeatureId < 0 ? cb : null;
    }

    /**
     * As AOSP's without panels: the callback, else the views. AOSP's then gives an unhandled key to the window's
     * own keys (volume, media, menu), which come with ADR-0041 a)'s input.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mWindow.isDestroyed()) {
            return false;
        }
        final Window.Callback cb = mWindow.getCallback();
        return cb != null && mFeatureId < 0 ? cb.dispatchKeyEvent(event) : super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchKeyShortcutEvent(KeyEvent ev) {
        final Window.Callback cb = callback();
        return cb != null ? cb.dispatchKeyShortcutEvent(ev) : super.dispatchKeyShortcutEvent(ev);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        final Window.Callback cb = callback();
        return cb != null ? cb.dispatchTouchEvent(ev) : super.dispatchTouchEvent(ev);
    }

    @Override
    public boolean dispatchTrackballEvent(MotionEvent ev) {
        final Window.Callback cb = callback();
        return cb != null ? cb.dispatchTrackballEvent(ev) : super.dispatchTrackballEvent(ev);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        final Window.Callback cb = callback();
        return cb != null ? cb.dispatchGenericMotionEvent(ev) : super.dispatchGenericMotionEvent(ev);
    }

    /** The view tree's own dispatch, which the activity's default dispatch comes back to through the window. */
    boolean superDispatchKeyEvent(KeyEvent event) {
        return super.dispatchKeyEvent(event);
    }

    boolean superDispatchKeyShortcutEvent(KeyEvent event) {
        return super.dispatchKeyShortcutEvent(event);
    }

    boolean superDispatchTouchEvent(MotionEvent event) {
        return super.dispatchTouchEvent(event);
    }

    boolean superDispatchTrackballEvent(MotionEvent event) {
        return super.dispatchTrackballEvent(event);
    }

    boolean superDispatchGenericMotionEvent(MotionEvent event) {
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        final Window.Callback cb = callback();
        if (cb != null) {
            cb.onWindowFocusChanged(hasWindowFocus);
        }
    }

    /** As AOSP's: the callback hears of it, then the window learns its root. */
    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        final Window.Callback cb = callback();
        if (cb != null) {
            cb.onAttachedToWindow();
        }
        mWindow.onViewRootImplSet(getViewRootImpl());
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        final Window.Callback cb = callback();
        if (cb != null) {
            cb.onDetachedFromWindow();
        }
        mPendingInsetsController.detach();
    }

    /** As AOSP's: the pending controller until the window is attached, then the window's own. */
    @Override
    public WindowInsetsController getWindowInsetsController() {
        if (isAttachedToWindow()) {
            return super.getWindowInsetsController();
        }
        return mPendingInsetsController;
    }

    /** ViewRootImpl.setView's, as AOSP's RootViewSurfaceTaker gives it. */
    public PendingInsetsController providePendingInsetsController() {
        return mPendingInsetsController;
    }
}
