/*
 * Our DecorView, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3): the root of an activity's window, a
 * FrameLayout that holds the window's layout (screen_simple's LinearLayout with the content FrameLayout) and hands
 * the window's events to its callback, the activity, as AOSP's DecorView does: key, touch and generic events, the
 * attach and detach, and the window's focus. Its background decides the window's default format, as AOSP's
 * drawableChanged does (ADR-0043 c3). AOSP's (2,670 lines) also draws the system bars' backgrounds, the caption, the
 * window's frame and the standalone and floating action modes, and resizes with the window; those come with ADR-0041
 * a)'s insets and ADR-0038's status bar. An action mode is the action bar's, which the activity gives it (the window
 * callback's onWindowStartingActionMode); a window without an action bar gets none.
 */
package com.android.internal.policy;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.ActionMode;
import android.view.InputQueue;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.PendingInsetsController;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import com.android.internal.widget.ActionBarContextView;
import com.android.internal.view.StandaloneActionMode;
import android.view.accessibility.AccessibilityEvent;
import android.view.ViewStub;

/** @hide */
public class DecorView extends FrameLayout {
    private final int mFeatureId;
    private PhoneWindow mWindow;
    /** The window's layout, the decor's only child. */
    ViewGroup mContentRoot;
    /** As AOSP's: what the app asks of the insets controller before the attach, which ViewRootImpl.setView replays. */
    private final PendingInsetsController mPendingInsetsController = new PendingInsetsController();
    /** As AOSP's: the window background's padding, which pads the decor. */
    private final Rect mBackgroundPadding = new Rect();
    /** The keyboard's ime() inset that pads the decor's bottom beside the background's (onApplyWindowInsets). */
    private int mImeBottom;

    /**
     * As AOSP's for a floating window's WRAP_CONTENT (ADR-0041 gap 4 (b)): the theme's fixed width and height, in
     * portrait the minor width and major height, and then its minimum width, a fraction of the screen's width in dp,
     * as a dialog gets them. Without insets (ADR-0041 decision 8) a floating window has no floating insets.
     */
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final DisplayMetrics metrics = getContext().getResources().getDisplayMetrics();
        final boolean isPortrait =
                getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT;
        final int widthMode = MeasureSpec.getMode(widthMeasureSpec);
        final int heightMode = MeasureSpec.getMode(heightMeasureSpec);
        boolean fixedWidth = false;
        if (widthMode == MeasureSpec.AT_MOST) {
            final TypedValue tvw = isPortrait ? mWindow.mFixedWidthMinor : mWindow.mFixedWidthMajor;
            final int w = size(tvw, metrics, metrics.widthPixels);
            if (w > 0) {
                widthMeasureSpec = MeasureSpec.makeMeasureSpec(Math.min(w, MeasureSpec.getSize(widthMeasureSpec)),
                        MeasureSpec.EXACTLY);
                fixedWidth = true;
            }
        }
        if (heightMode == MeasureSpec.AT_MOST) {
            final TypedValue tvh = isPortrait ? mWindow.mFixedHeightMajor : mWindow.mFixedHeightMinor;
            final int h = size(tvh, metrics, metrics.heightPixels);
            if (h > 0) {
                heightMeasureSpec = MeasureSpec.makeMeasureSpec(Math.min(h, MeasureSpec.getSize(heightMeasureSpec)),
                        MeasureSpec.EXACTLY);
            }
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
        final int width = getMeasuredWidth();
        if (!fixedWidth && widthMode == MeasureSpec.AT_MOST) {
            final TypedValue tv = isPortrait ? mWindow.mMinWidthMinor : mWindow.mMinWidthMajor;
            final float availableWidth = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                    getResources().getConfiguration().screenWidthDp, metrics);
            int min = 0;
            if (tv.type == TypedValue.TYPE_DIMENSION) {
                min = (int) tv.getDimension(metrics);
            } else if (tv.type == TypedValue.TYPE_FRACTION) {
                min = (int) tv.getFraction(availableWidth, availableWidth);
            }
            if (width < min) {
                super.onMeasure(MeasureSpec.makeMeasureSpec(min, MeasureSpec.EXACTLY), heightMeasureSpec);
            }
        }
    }

    /** A fixed size of the theme: a dimension, or a fraction of `whole`; 0 for none. */
    private static int size(TypedValue value, DisplayMetrics metrics, int whole) {
        if (value == null || value.type == TypedValue.TYPE_NULL) {
            return 0;
        }
        if (value.type == TypedValue.TYPE_DIMENSION) {
            return (int) value.getDimension(metrics);
        }
        if (value.type == TypedValue.TYPE_FRACTION) {
            return (int) value.getFraction(whole, whole);
        }
        return 0;
    }

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

    /**
     * As AOSP's: the window's background, which the decor draws behind everything, and with a new one the window's
     * default format (drawableChanged).
     */
    @SuppressWarnings("deprecation") // View.setBackgroundDrawable, as AOSP's updateBackgroundDrawable calls it.
    public void setWindowBackground(Drawable drawable) {
        if (getBackground() != drawable) {
            super.setBackgroundDrawable(drawable);
            if (drawable != null) {
                drawable.getPadding(mBackgroundPadding);
            } else {
                mBackgroundPadding.setEmpty();
            }
            if (mImeBottom != 0) {
                padBottom();
            }
            if (mWindow != null) {
                drawableChanged();
            }
        }
    }

    /** As AOSP's: a background set on the decor is the window's. */
    @Override
    @SuppressWarnings("deprecation") // View.setBackgroundDrawable, which AOSP's DecorView overrides too.
    public void setBackgroundDrawable(Drawable background) {
        setWindowBackground(background);
    }

    /**
     * As AOSP's drawableChanged for a window without a frame or a shadow, which ours never have: the window's default
     * format, OPAQUE without a background, else the background's opacity, or with a foreground the two together. AOSP's
     * also pads the decor by the frame's and the background's padding; View's setBackgroundDrawable takes the latter.
     */
    @SuppressWarnings("deprecation") // Drawable.getOpacity, as AOSP's drawableChanged calls it.
    private void drawableChanged() {
        int opacity = PixelFormat.OPAQUE;
        final Drawable bg = getBackground();
        final Drawable fg = getForeground();
        if (bg != null) {
            if (fg == null) {
                opacity = bg.getOpacity();
            } else {
                // Without a frame the frame's padding is zero, so either one opaque makes the window opaque.
                final int fop = fg.getOpacity();
                final int bop = bg.getOpacity();
                if (fop == PixelFormat.OPAQUE || bop == PixelFormat.OPAQUE) {
                    opacity = PixelFormat.OPAQUE;
                } else if (fop == PixelFormat.UNKNOWN) {
                    opacity = bop;
                } else if (bop == PixelFormat.UNKNOWN) {
                    opacity = fop;
                } else {
                    opacity = Drawable.resolveOpacity(fop, bop);
                }
            }
        }
        if (mFeatureId < 0) {
            mWindow.setDefaultWindowFormat(opacity);
        }
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
     * As AOSP's: a panel key that is held lets the next key try a shortcut in the panel, an open panel is given the
     * key as a shortcut, then the callback or the views, and a key they leave goes to the window's own keys (the MENU
     * key, which opens the options panel).
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int keyCode = event.getKeyCode();
        final int action = event.getAction();
        final boolean isDown = action == KeyEvent.ACTION_DOWN;

        if (isDown && (event.getRepeatCount() == 0)) {
            // First handle chording of panel key: if a panel key is held but not released, try to execute a shortcut in
            // it.
            if ((mWindow.mPanelChordingKey > 0) && (mWindow.mPanelChordingKey != keyCode)) {
                boolean handled = dispatchKeyShortcutEvent(event);
                if (handled) {
                    return true;
                }
            }

            // If a panel is open, perform a shortcut on it without the chorded panel key
            if ((mWindow.mPreparedPanel != null) && mWindow.mPreparedPanel.isOpen) {
                if (mWindow.performPanelShortcut(mWindow.mPreparedPanel, keyCode, event, 0)) {
                    return true;
                }
            }
        }

        if (!mWindow.isDestroyed()) {
            final Window.Callback cb = mWindow.getCallback();
            final boolean handled = cb != null && mFeatureId < 0 ? cb.dispatchKeyEvent(event)
                    : super.dispatchKeyEvent(event);
            if (handled) {
                return true;
            }
        }

        return isDown ? mWindow.onKeyDown(mFeatureId, event.getKeyCode(), event)
                : mWindow.onKeyUp(mFeatureId, event.getKeyCode(), event);
    }

    /** As AOSP's: the prepared panel first, then the callback or the views, then a panel prepared for the shortcut. */
    @Override
    public boolean dispatchKeyShortcutEvent(KeyEvent ev) {
        // If the panel is already prepared, then perform the shortcut using it.
        boolean handled;
        if (mWindow.mPreparedPanel != null) {
            handled = mWindow.performPanelShortcut(mWindow.mPreparedPanel, ev.getKeyCode(), ev,
                    Menu.FLAG_PERFORM_NO_CLOSE);
            if (handled) {
                if (mWindow.mPreparedPanel != null) {
                    mWindow.mPreparedPanel.isHandled = true;
                }
                return true;
            }
        }

        // Shortcut not handled by the panel. Dispatch to the view hierarchy.
        final Window.Callback cb = mWindow.getCallback();
        handled = cb != null && !mWindow.isDestroyed() && mFeatureId < 0
                ? cb.dispatchKeyShortcutEvent(ev) : super.dispatchKeyShortcutEvent(ev);
        if (handled) {
            return true;
        }

        // If the panel is not prepared, then we may be trying to handle a shortcut key combination such as Control+C.
        // Temporarily prepare the panel then mark it unprepared again when finished to ensure that the panel will
        // again be prepared the next time it is shown for real.
        PhoneWindow.PanelFeatureState st = mWindow.getPanelState(Window.FEATURE_OPTIONS_PANEL, false);
        if (st != null && mWindow.mPreparedPanel == null) {
            mWindow.preparePanel(st, ev);
            handled = mWindow.performPanelShortcut(st, ev.getKeyCode(), ev, Menu.FLAG_PERFORM_NO_CLOSE);
            st.isPrepared = false;
            if (handled) {
                return true;
            }
        }
        return false;
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

    /** ViewRootImpl's: whether the window is floating (a dialog's or a floating activity's theme). */
    public boolean isFloatingWindow() {
        return mWindow != null && mWindow.isFloating();
    }

    /**
     * The view tree's own dispatch, which the activity's default dispatch comes back to through the window; for a key,
     * then the unhandled-key listeners of the window's views, as AOSP's but for action modes (ADR-0045 step 7c1).
     */
    boolean superDispatchKeyEvent(KeyEvent event) {
        // As AOSP's: a primary action mode takes the BACK key before any view and ends with its up.
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && mPrimaryActionMode != null) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                mPrimaryActionMode.finish();
            }
            return true;
        }
        if (super.dispatchKeyEvent(event)) {
            return true;
        }
        return (getViewRootImpl() != null) && getViewRootImpl().dispatchUnhandledKeyEvent(event);
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

    /**
     * As Android's for a window that is not edge to edge (target SDK below 35 or the theme's opt-out) with
     * SOFT_INPUT_ADJUST_RESIZE: the keyboard's ime() inset takes room from the content, whose views are laid out above
     * the keyboard. On Android the window manager shrinks the window's frame; here the decor pads its bottom, which
     * lays out and draws the same (ADR-0068 step 2d), below the background's padding, which a floating window's
     * background has. Every other window leaves the inset to its views, as one that is edge to edge does. The views
     * still get the ime() inset in the insets they apply.
     */
    @Override
    @SuppressWarnings("deprecation")
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        final int adjust = mWindow.getAttributes().softInputMode & WindowManager.LayoutParams.SOFT_INPUT_MASK_ADJUST;
        final boolean resize = !mWindow.isEdgeToEdgeEnforced()
                && adjust == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
        final int ime = resize ? insets.getInsets(WindowInsets.Type.ime()).bottom : 0;
        if (ime != mImeBottom) {
            mImeBottom = ime;
            padBottom();
        }
        return super.onApplyWindowInsets(insets);
    }

    /** The decor's bottom padding: the background's, as AOSP's drawableChanged sets it, and the keyboard's. */
    private void padBottom() {
        setPadding(getPaddingLeft(), getPaddingTop(), getPaddingRight(), mBackgroundPadding.bottom + mImeBottom);
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

    /**
     * ViewRootImpl.setView's, as AOSP's RootViewSurfaceTaker gives it: the callback that takes the window's surface
     * (NativeActivity's, Window.takeSurface; ADR-0069 step 2), for the window's own decor only.
     */
    public SurfaceHolder.Callback2 willYouTakeTheSurface() {
        return mFeatureId < 0 ? mWindow.mTakeSurfaceCallback : null;
    }

    /**
     * ViewRootImpl.setView's, as AOSP's RootViewSurfaceTaker gives it: the callback that takes the window's input queue
     * (NativeActivity's, Window.takeInputQueue; ADR-0069 step 3), for the window's own decor only.
     */
    public InputQueue.Callback willYouTakeTheInputQueue() {
        return mFeatureId < 0 ? mWindow.mTakeInputQueueCallback : null;
    }

    /** As AOSP's RootViewSurfaceTaker.setSurfaceFormat: a taken surface's format is the window's (Window.setFormat). */
    public void setSurfaceFormat(int format) {
        mWindow.setFormat(format);
    }

    /** As AOSP's: the primary action mode of the action bar, which the window callback hands out. */
    ActionMode mPrimaryActionMode;
    /** The standalone action mode's bar, inflated from the window's action_mode_bar_stub on its first mode. */
    private ActionBarContextView mPrimaryActionModeView;
    private ObjectAnimator mFadeAnim;

    @Override
    public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback) {
        return startActionModeForChild(originalView, callback, ActionMode.TYPE_PRIMARY);
    }

    @Override
    public ActionMode startActionModeForChild(View child, ActionMode.Callback callback, int type) {
        return startActionMode(callback, type);
    }

    @Override
    public ActionMode startActionMode(ActionMode.Callback callback) {
        return startActionMode(callback, ActionMode.TYPE_PRIMARY);
    }

    /**
     * As AOSP's startActionMode: the activity's action bar starts the mode, and a window without one shows it in its
     * own standalone bar (the action_mode_bar_stub); the floating mode and the standalone bar of a floating window come
     * with the popups (ADR-0041 a)).
     */
    @Override
    public ActionMode startActionMode(ActionMode.Callback callback, int type) {
        final ActionMode.Callback2 wrappedCallback = new ActionModeCallback2Wrapper(callback);
        ActionMode mode = null;
        if (mWindow.getCallback() != null && !mWindow.isDestroyed()) {
            try {
                mode = mWindow.getCallback().onWindowStartingActionMode(wrappedCallback, type);
            } catch (AbstractMethodError ame) {
                // Older apps might not implement the typed version of this method.
                if (type == ActionMode.TYPE_PRIMARY) {
                    try {
                        mode = mWindow.getCallback().onWindowStartingActionMode(wrappedCallback);
                    } catch (AbstractMethodError ame2) {
                        // Older apps might not implement this callback method at all.
                    }
                }
            }
        }
        if (mode != null) {
            if (mode.getType() == ActionMode.TYPE_PRIMARY) {
                if (mPrimaryActionMode != null) {
                    mPrimaryActionMode.finish();
                }
                mPrimaryActionMode = mode;
            }
        } else if (type == ActionMode.TYPE_PRIMARY) {
            // As AOSP's: the window's own standalone bar when no action bar starts the mode.
            mode = createStandaloneActionMode(wrappedCallback);
            if (mode != null && wrappedCallback.onCreateActionMode(mode, mode.getMenu())) {
                setHandledPrimaryActionMode(mode);
            } else {
                mode = null;
            }
        }
        if (mode != null && mWindow.getCallback() != null && !mWindow.isDestroyed()) {
            try {
                mWindow.getCallback().onActionModeStarted(mode);
            } catch (AbstractMethodError ame) {
                // Older apps might not implement this callback method.
            }
        }
        return mode;
    }

    /** As AOSP's createStandaloneActionMode for a window that is not floating (the stub's bar). */
    private ActionMode createStandaloneActionMode(ActionMode.Callback callback) {
        endOnGoingFadeAnimation();
        if (mPrimaryActionMode != null) {
            mPrimaryActionMode.finish();
            mPrimaryActionMode = null;
        }
        if (mPrimaryActionModeView != null) {
            mPrimaryActionModeView.killMode();
        }
        if ((mPrimaryActionModeView == null || !mPrimaryActionModeView.isAttachedToWindow()) && !mWindow.isFloating()) {
            final ViewStub stub = findViewById(com.android.internal.R.id.action_mode_bar_stub);
            if (stub != null) {
                mPrimaryActionModeView = (ActionBarContextView) stub.inflate();
            }
        }
        if (mPrimaryActionModeView != null) {
            mPrimaryActionModeView.killMode();
            return new StandaloneActionMode(mPrimaryActionModeView.getContext(), mPrimaryActionModeView, callback,
                    true);
        }
        return null;
    }

    /** As AOSP's setHandledPrimaryActionMode for the stub's bar. */
    private void setHandledPrimaryActionMode(ActionMode mode) {
        endOnGoingFadeAnimation();
        mPrimaryActionMode = mode;
        mPrimaryActionMode.invalidate();
        mPrimaryActionModeView.initForMode(mPrimaryActionMode);
        if (isLaidOut()) {
            mFadeAnim = ObjectAnimator.ofFloat(mPrimaryActionModeView, View.ALPHA, 0f, 1f);
            mFadeAnim.addListener(new AnimatorListenerAdapter() {
                @Override
                public void onAnimationStart(Animator animation) {
                    mPrimaryActionModeView.setVisibility(View.VISIBLE);
                }

                @Override
                public void onAnimationEnd(Animator animation) {
                    mPrimaryActionModeView.setAlpha(1f);
                    mFadeAnim = null;
                }
            });
            mFadeAnim.start();
        } else {
            mPrimaryActionModeView.setAlpha(1f);
            mPrimaryActionModeView.setVisibility(View.VISIBLE);
        }
        mPrimaryActionModeView.sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED);
    }

    private void endOnGoingFadeAnimation() {
        if (mFadeAnim != null) {
            mFadeAnim.end();
        }
    }

    /** As AOSP's: clears the references when the action mode is destroyed. */
    private class ActionModeCallback2Wrapper extends ActionMode.Callback2 {
        private final ActionMode.Callback mWrapped;

        ActionModeCallback2Wrapper(ActionMode.Callback wrapped) {
            mWrapped = wrapped;
        }

        public boolean onCreateActionMode(ActionMode mode, Menu menu) {
            return mWrapped.onCreateActionMode(mode, menu);
        }

        public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
            return mWrapped.onPrepareActionMode(mode, menu);
        }

        public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
            return mWrapped.onActionItemClicked(mode, item);
        }

        public void onDestroyActionMode(ActionMode mode) {
            mWrapped.onDestroyActionMode(mode);
            if (mPrimaryActionModeView != null) {
                endOnGoingFadeAnimation();
                mFadeAnim = ObjectAnimator.ofFloat(mPrimaryActionModeView, View.ALPHA, 0f);
                mFadeAnim.addListener(new AnimatorListenerAdapter() {
                    @Override
                    public void onAnimationEnd(Animator animation) {
                        mPrimaryActionModeView.setVisibility(View.GONE);
                        if (mPrimaryActionModeView.getParent() instanceof View) {
                            ((View) mPrimaryActionModeView.getParent()).requestApplyInsets();
                        }
                        mPrimaryActionModeView.killMode();
                        mFadeAnim = null;
                    }
                });
                mFadeAnim.start();
            }
            if (mode == mPrimaryActionMode) {
                mPrimaryActionMode = null;
            }
            if (mWindow.getCallback() != null && !mWindow.isDestroyed()) {
                try {
                    mWindow.getCallback().onActionModeFinished(mode);
                } catch (AbstractMethodError ame) {
                    // Older apps might not implement this callback method.
                }
            }
        }

        @Override
        public void onGetContentRect(ActionMode mode, View view, Rect outRect) {
            if (mWrapped instanceof ActionMode.Callback2) {
                ((ActionMode.Callback2) mWrapped).onGetContentRect(mode, view, outRect);
            } else {
                super.onGetContentRect(mode, view, outRect);
            }
        }
    }
}
