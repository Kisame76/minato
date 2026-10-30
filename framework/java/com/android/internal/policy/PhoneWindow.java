/*
 * Our PhoneWindow, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3), which Activity.attach makes for every
 * activity. As AOSP's it installs the decor at the first setContentView or getDecorView: a DecorView with the layout
 * the theme's window style asks for, whose FrameLayout is android.R.id.content: without FEATURE_NO_TITLE a floating
 * window's dialogTitleDecorLayout and any other's screen_title, with the title in android.R.id.title, else
 * screen_simple (or screen_simple_overlay_action_mode); then it hands content changes to the activity, keeps the
 * window's flags, title, background and soft input mode, and saves and restores the content's view state. Its
 * differences from AOSP's (4,288 lines):
 *   - no action bar: a theme with one (windowActionBar) gets screen_simple, so getActionBar() is null and the content
 *     has the action bar's height more (a listed gap); no title icons, progress or custom title (FEATURE_LEFT_ICON,
 *     FEATURE_PROGRESS, FEATURE_CUSTOM_TITLE); options-menu panels open nothing;
 *   - edge-to-edge is enforced from target SDK 35 unless the theme opts out (windowOptOutEdgeToEdgeEnforcement), as
 *     Android 15 does through the compat change ENFORCE_EDGE_TO_EDGE, which only a developer override changes;
 *     Minato's in-process compat checks answer every change as enabled until the app's disabled changes are
 *     installed (a listed gap), so ours reads the target SDK directly;
 *   - the system bars' colors are kept, not drawn, and the insets controller is none until ADR-0041 decision 8;
 *   - the decor's context is the activity, where AOSP's is a DecorContext over the application.
 */
package com.android.internal.policy;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.TypedArray;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Parcelable;
import android.text.TextUtils;
import android.util.Log;
import android.util.Pair;
import android.util.SparseArray;
import android.util.TypedValue;
import android.view.AttachedSurfaceControl;
import android.view.InputEvent;
import android.view.InputQueue;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.Window;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.window.OnBackInvokedDispatcher;
import android.widget.TextView;
import android.window.ProxyOnBackInvokedDispatcher;
import com.android.internal.R;
import java.util.List;

/** @hide */
@SuppressWarnings("deprecation") // Window's bar colors, flags and insets that it implements and keeps, as AOSP's.
public class PhoneWindow extends Window {
    private static final String TAG = "PhoneWindow";
    private static final String FOCUSED_ID_TAG = "android:focusedViewId";
    private static final String VIEWS_TAG = "android:views";

    private static final int SYSTEM_UI_LAYOUT_FLAGS = View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;

    /** As AOSP's: a content view that fits the system windows takes their insets as padding. */
    private static final OnContentApplyWindowInsetsListener sDefaultContentInsetsApplier = (view, insets) -> {
        if ((view.getWindowSystemUiVisibility() & SYSTEM_UI_LAYOUT_FLAGS) != 0) {
            return new Pair<>(Insets.NONE, insets);
        }
        Insets insetsToApply = insets.getSystemWindowInsets();
        return new Pair<>(insetsToApply, insets.inset(insetsToApply).consumeSystemWindowInsets());
    };

    private final LayoutInflater mLayoutInflater;
    private final ViewRootImpl.ActivityConfigCallback mActivityConfigCallback;
    private final ProxyOnBackInvokedDispatcher mProxyOnBackInvokedDispatcher;
    private DecorView mDecor;
    private ViewGroup mContentParent;
    private boolean mContentParentExplicitlySet;
    private CharSequence mTitle;
    private int mTitleColor;
    /** The title bar's text, android.R.id.title of the decor's layout, null without a title bar. */
    private TextView mTitleView;
    private boolean mIsFloating;
    /** As AOSP's: the theme's minimum and fixed sizes of a floating window, which DecorView.onMeasure applies. */
    final TypedValue mMinWidthMajor = new TypedValue();
    final TypedValue mMinWidthMinor = new TypedValue();
    TypedValue mFixedWidthMajor;
    TypedValue mFixedWidthMinor;
    TypedValue mFixedHeightMajor;
    TypedValue mFixedHeightMinor;
    private boolean mDecorFitsSystemWindows = true;
    private boolean mEdgeToEdgeEnforced;

    /** Whether the window is edge to edge by the platform's rule (target SDK 35 unless the theme opts out). */
    boolean isEdgeToEdgeEnforced() {
        return mEdgeToEdgeEnforced;
    }
    private Drawable mBackgroundDrawable;
    private int mBackgroundResource;
    private int mStatusBarColor;
    private int mNavigationBarColor;
    private int mVolumeControlStreamType = AudioManager.USE_DEFAULT_STREAM_TYPE;
    /** What NativeActivity takes, which ADR-0041 a)'s surfaces and input hand over. */
    SurfaceHolder.Callback2 mTakeSurfaceCallback;
    InputQueue.Callback mTakeInputQueueCallback;

    public PhoneWindow(Context context) {
        this(context, null, null);
    }

    public PhoneWindow(Context context, Window preservedWindow,
            ViewRootImpl.ActivityConfigCallback activityConfigCallback) {
        super(context);
        mLayoutInflater = LayoutInflater.from(context);
        mProxyOnBackInvokedDispatcher = new ProxyOnBackInvokedDispatcher(context);
        mActivityConfigCallback = activityConfigCallback;
    }

    // The decor and the content.

    private void installDecor() {
        if (mDecor == null) {
            mDecor = new DecorView(getContext(), -1, this, getAttributes());
            mDecor.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
            mDecor.setIsRootNamespace(true);
        }
        if (mContentParent == null) {
            mContentParent = generateLayout(mDecor);
            mDecor.makeFrameworkOptionalFitsSystemWindows();
        }
    }

    /** As AOSP's generateLayout, for the features and attributes ours has: the window's style, then its layout. */
    private ViewGroup generateLayout(DecorView decor) {
        TypedArray a = getWindowStyle();
        mIsFloating = a.getBoolean(R.styleable.Window_windowIsFloating, false);
        int flagsToUpdate = (WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_INSET_DECOR) & ~getForcedWindowFlags();
        if (mIsFloating) {
            setLayout(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            setFlags(0, flagsToUpdate);
        } else {
            setFlags(WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_INSET_DECOR, flagsToUpdate);
            getAttributes().setFitInsetsSides(0);
            getAttributes().setFitInsetsTypes(0);
        }
        if (a.getBoolean(R.styleable.Window_windowNoTitle, false)) {
            requestFeature(FEATURE_NO_TITLE);
        }
        if (a.getBoolean(R.styleable.Window_windowActionModeOverlay, false)) {
            requestFeature(FEATURE_ACTION_MODE_OVERLAY);
        }
        if (a.getBoolean(R.styleable.Window_windowFullscreen, false)) {
            setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                    WindowManager.LayoutParams.FLAG_FULLSCREEN & ~getForcedWindowFlags());
        }
        if (a.getBoolean(R.styleable.Window_windowShowWallpaper, false)) {
            setFlags(WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER,
                    WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER & ~getForcedWindowFlags());
        }
        if (a.getBoolean(R.styleable.Window_windowEnableSplitTouch,
                getContext().getApplicationInfo().targetSdkVersion >= Build.VERSION_CODES.HONEYCOMB)) {
            setFlags(WindowManager.LayoutParams.FLAG_SPLIT_TOUCH,
                    WindowManager.LayoutParams.FLAG_SPLIT_TOUCH & ~getForcedWindowFlags());
        }
        // As AOSP's: a floating window's sizes, and every dialog's dim (ADR-0041 gap 4 (b)).
        a.getValue(R.styleable.Window_windowMinWidthMajor, mMinWidthMajor);
        a.getValue(R.styleable.Window_windowMinWidthMinor, mMinWidthMinor);
        if (a.hasValue(R.styleable.Window_windowFixedWidthMajor)) {
            if (mFixedWidthMajor == null) {
                mFixedWidthMajor = new TypedValue();
            }
            a.getValue(R.styleable.Window_windowFixedWidthMajor, mFixedWidthMajor);
        }
        if (a.hasValue(R.styleable.Window_windowFixedWidthMinor)) {
            if (mFixedWidthMinor == null) {
                mFixedWidthMinor = new TypedValue();
            }
            a.getValue(R.styleable.Window_windowFixedWidthMinor, mFixedWidthMinor);
        }
        if (a.hasValue(R.styleable.Window_windowFixedHeightMajor)) {
            if (mFixedHeightMajor == null) {
                mFixedHeightMajor = new TypedValue();
            }
            a.getValue(R.styleable.Window_windowFixedHeightMajor, mFixedHeightMajor);
        }
        if (a.hasValue(R.styleable.Window_windowFixedHeightMinor)) {
            if (mFixedHeightMinor == null) {
                mFixedHeightMinor = new TypedValue();
            }
            a.getValue(R.styleable.Window_windowFixedHeightMinor, mFixedHeightMinor);
        }
        if (a.getBoolean(R.styleable.Window_backgroundDimEnabled, mIsFloating)) {
            WindowManager.LayoutParams dimmed = getAttributes();
            if ((getForcedWindowFlags() & WindowManager.LayoutParams.FLAG_DIM_BEHIND) == 0) {
                dimmed.flags |= WindowManager.LayoutParams.FLAG_DIM_BEHIND;
            }
            if (!haveDimAmount()) {
                dimmed.dimAmount = a.getFloat(android.R.styleable.Window_backgroundDimAmount, 0.5f);
            }
        }
        mEdgeToEdgeEnforced = !a.getBoolean(R.styleable.Window_windowOptOutEdgeToEdgeEnforcement, false)
                && getContext().getApplicationInfo().targetSdkVersion >= Build.VERSION_CODES.VANILLA_ICE_CREAM;
        if (mEdgeToEdgeEnforced) {
            getAttributes().privateFlags |= WindowManager.LayoutParams.PRIVATE_FLAG_EDGE_TO_EDGE_ENFORCED;
            mDecorFitsSystemWindows = false;
            mStatusBarColor = Color.TRANSPARENT;
        } else {
            mStatusBarColor = a.getColor(R.styleable.Window_statusBarColor, Color.BLACK);
        }
        mNavigationBarColor = a.getColor(R.styleable.Window_navigationBarColor, Color.BLACK);
        WindowManager.LayoutParams params = getAttributes();
        if (!hasSoftInputMode()) {
            params.softInputMode = a.getInt(R.styleable.Window_windowSoftInputMode, params.softInputMode);
        }
        if (params.windowAnimations == 0) {
            params.windowAnimations = a.getResourceId(R.styleable.Window_windowAnimationStyle, 0);
        }
        if (getContainer() == null && mBackgroundDrawable == null && mBackgroundResource == 0) {
            mBackgroundResource = a.getResourceId(R.styleable.Window_windowBackground, 0);
        }
        // As AOSP's: a title bar unless FEATURE_NO_TITLE, the dialog's for a floating window; a theme with an action
        // bar keeps screen_simple, our gap.
        final int features = getLocalFeatures();
        final int layoutResource;
        if ((features & (1 << FEATURE_NO_TITLE)) == 0 && mIsFloating) {
            TypedValue res = new TypedValue();
            getContext().getTheme().resolveAttribute(R.attr.dialogTitleDecorLayout, res, true);
            layoutResource = res.resourceId;
        } else if ((features & (1 << FEATURE_NO_TITLE)) == 0
                && !a.getBoolean(R.styleable.Window_windowActionBar, false)) {
            layoutResource = R.layout.screen_title;
        } else if ((features & (1 << FEATURE_ACTION_MODE_OVERLAY)) != 0) {
            layoutResource = R.layout.screen_simple_overlay_action_mode;
        } else {
            layoutResource = R.layout.screen_simple;
        }
        decor.onResourcesLoaded(mLayoutInflater, layoutResource);
        ViewGroup contentParent = (ViewGroup) findViewById(ID_ANDROID_CONTENT);
        if (contentParent == null) {
            throw new RuntimeException("Window couldn't find content container view");
        }
        mTitleView = (TextView) findViewById(R.id.title);
        if (mTitleView != null) {
            mTitleView.setText(mTitle);
            if (mTitleColor != 0) {
                mTitleView.setTextColor(mTitleColor);
            }
        }
        if (getContainer() == null) {
            decor.setWindowBackground(mBackgroundResource != 0
                    ? getContext().getDrawable(mBackgroundResource) : mBackgroundDrawable);
            if (mTitle != null) {
                setTitle(mTitle);
            }
        }
        return contentParent;
    }

    @Override
    public boolean requestFeature(int featureId) {
        if (mContentParentExplicitlySet) {
            throw new android.util.AndroidRuntimeException("requestFeature() must be called before adding content");
        }
        return super.requestFeature(featureId);
    }

    @Override
    public void setContentView(int layoutResID) {
        if (mContentParent == null) {
            installDecor();
        } else {
            mContentParent.removeAllViews();
        }
        mLayoutInflater.inflate(layoutResID, mContentParent);
        onContentChanged();
    }

    @Override
    public void setContentView(View view) {
        setContentView(view, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        if (mContentParent == null) {
            installDecor();
        } else {
            mContentParent.removeAllViews();
        }
        mContentParent.addView(view, params);
        onContentChanged();
    }

    @Override
    public void addContentView(View view, ViewGroup.LayoutParams params) {
        if (mContentParent == null) {
            installDecor();
        }
        mContentParent.addView(view, params);
        mContentParent.requestApplyInsets();
        final Callback cb = getCallback();
        if (cb != null && !isDestroyed()) {
            cb.onContentChanged();
        }
    }

    /** As AOSP's after a setContentView: the insets again, the activity's onContentChanged. */
    private void onContentChanged() {
        mContentParent.requestApplyInsets();
        final Callback cb = getCallback();
        if (cb != null && !isDestroyed()) {
            cb.onContentChanged();
        }
        mContentParentExplicitlySet = true;
    }

    @Override
    public void clearContentView() {
        if (mDecor != null) {
            mDecor.clearContentView();
        }
    }

    @Override
    public View getDecorView() {
        if (mDecor == null) {
            installDecor();
        }
        return mDecor;
    }

    /** As AOSP's: the decor view's insets controller, a pending one until the window is attached. */
    @Override
    public WindowInsetsController getInsetsController() {
        return mDecor.getWindowInsetsController();
    }

    @Override
    public final View peekDecorView() {
        return mDecor;
    }

    @Override
    public View getCurrentFocus() {
        return mDecor != null ? mDecor.findFocus() : null;
    }

    @Override
    public LayoutInflater getLayoutInflater() {
        return mLayoutInflater;
    }

    @Override
    public boolean isFloating() {
        return mIsFloating;
    }

    /** As AOSP's: what the window's root needs once ViewRootImpl holds the decor. */
    void onViewRootImplSet(ViewRootImpl viewRoot) {
        viewRoot.setActivityConfigCallback(mActivityConfigCallback);
        viewRoot.getOnBackInvokedDispatcher().updateContext(getContext());
        mProxyOnBackInvokedDispatcher.setActualDispatcher(viewRoot.getOnBackInvokedDispatcher());
        applyDecorFitsSystemWindows();
    }

    private ViewRootImpl getViewRootImplOrNull() {
        View decorView = peekDecorView();
        return decorView != null ? decorView.getViewRootImpl() : null;
    }

    @Override
    public AttachedSurfaceControl getRootSurfaceControl() {
        return getViewRootImplOrNull();
    }

    @Override
    public OnBackInvokedDispatcher getOnBackInvokedDispatcher() {
        return mProxyOnBackInvokedDispatcher;
    }

    @Override
    public void setSystemGestureExclusionRects(List<Rect> rects) {
        ViewRootImpl viewRoot = getViewRootImplOrNull();
        if (viewRoot == null) {
            throw new IllegalStateException("Window is not attached");
        }
        viewRoot.setRootSystemGestureExclusionRects(rects);
    }

    @Override
    public List<Rect> getSystemGestureExclusionRects() {
        ViewRootImpl viewRoot = getViewRootImplOrNull();
        if (viewRoot == null) {
            throw new IllegalStateException("Window is not attached");
        }
        return viewRoot.getRootSystemGestureExclusionRects();
    }

    @Override
    public void setDecorFitsSystemWindows(boolean decorFitsSystemWindows) {
        if (mEdgeToEdgeEnforced) {
            return;
        }
        mDecorFitsSystemWindows = decorFitsSystemWindows;
        applyDecorFitsSystemWindows();
    }

    @Override
    public boolean decorFitsSystemWindows() {
        return mDecorFitsSystemWindows;
    }

    private void applyDecorFitsSystemWindows() {
        ViewRootImpl impl = getViewRootImplOrNull();
        if (impl != null) {
            impl.setOnContentApplyWindowInsetsListener(mDecorFitsSystemWindows ? sDefaultContentInsetsApplier : null);
        }
    }

    // The title, the background and the system bars' colors.

    @Override
    public void setTitle(CharSequence title) {
        mTitle = title;
        if (mTitleView != null) {
            mTitleView.setText(title);
        }
        WindowManager.LayoutParams params = getAttributes();
        if (!TextUtils.equals(title, params.accessibilityTitle)) {
            params.accessibilityTitle = TextUtils.stringOrSpannedString(title);
            ViewRootImpl viewRoot = getViewRootImplOrNull();
            if (viewRoot != null) {
                viewRoot.onWindowTitleChanged();
            }
            dispatchWindowAttributesChanged(getAttributes());
        }
    }

    @Override
    @Deprecated
    public void setTitleColor(int textColor) {
        mTitleColor = textColor;
        if (mTitleView != null) {
            mTitleView.setTextColor(textColor);
        }
    }

    /** As AOSP's: here, so that DecorView, in this package, sets the window's default format (ADR-0043 c3). */
    @Override
    protected void setDefaultWindowFormat(int format) {
        super.setDefaultWindowFormat(format);
    }

    @Override
    public void setBackgroundDrawable(Drawable drawable) {
        if (drawable != mBackgroundDrawable || mBackgroundResource != 0) {
            mBackgroundResource = 0;
            mBackgroundDrawable = drawable;
            if (mDecor != null) {
                mDecor.setWindowBackground(drawable);
            }
        }
    }

    @Override
    public int getStatusBarColor() {
        return mStatusBarColor;
    }

    @Override
    public void setStatusBarColor(int color) {
        if (!mEdgeToEdgeEnforced) {
            mStatusBarColor = color;
        }
    }

    @Override
    public int getNavigationBarColor() {
        return mEdgeToEdgeEnforced ? Color.TRANSPARENT : mNavigationBarColor;
    }

    @Override
    public void setNavigationBarColor(int color) {
        mNavigationBarColor = color;
    }

    // Saved state: the content's views and focus, as AOSP's without panels and action bar.

    @Override
    public Bundle saveHierarchyState() {
        Bundle outState = new Bundle();
        if (mContentParent == null) {
            return outState;
        }
        SparseArray<Parcelable> states = new SparseArray<>();
        mContentParent.saveHierarchyState(states);
        outState.putSparseParcelableArray(VIEWS_TAG, states);
        final View focusedView = mContentParent.findFocus();
        if (focusedView != null && focusedView.getId() != View.NO_ID) {
            outState.putInt(FOCUSED_ID_TAG, focusedView.getId());
        }
        return outState;
    }

    @Override
    public void restoreHierarchyState(Bundle savedInstanceState) {
        if (mContentParent == null) {
            return;
        }
        SparseArray<Parcelable> savedStates = savedInstanceState.getSparseParcelableArray(VIEWS_TAG,
                Parcelable.class);
        if (savedStates != null) {
            mContentParent.restoreHierarchyState(savedStates);
        }
        int focusedViewId = savedInstanceState.getInt(FOCUSED_ID_TAG, View.NO_ID);
        if (focusedViewId != View.NO_ID) {
            View needsFocus = mContentParent.findViewById(focusedViewId);
            if (needsFocus != null) {
                needsFocus.requestFocus();
            } else {
                Log.w(TAG, "Previously focused view reported id " + focusedViewId
                        + " during save, but can't be found during restore.");
            }
        }
    }

    // The view tree's own dispatch, for Activity's defaults.

    @Override
    public boolean superDispatchKeyEvent(KeyEvent event) {
        return mDecor.superDispatchKeyEvent(event);
    }

    @Override
    public boolean superDispatchKeyShortcutEvent(KeyEvent event) {
        return mDecor.superDispatchKeyShortcutEvent(event);
    }

    @Override
    public boolean superDispatchTouchEvent(MotionEvent event) {
        return mDecor.superDispatchTouchEvent(event);
    }

    @Override
    public boolean superDispatchTrackballEvent(MotionEvent event) {
        return mDecor.superDispatchTrackballEvent(event);
    }

    @Override
    public boolean superDispatchGenericMotionEvent(MotionEvent event) {
        return mDecor.superDispatchGenericMotionEvent(event);
    }

    /** As AOSP's: an event the app injects goes through the window's input stages (ADR-0045 step 7c1). */
    @Override
    public void injectInputEvent(InputEvent event) {
        getViewRootImpl().dispatchInputEvent(event);
    }

    private ViewRootImpl getViewRootImpl() {
        final ViewRootImpl viewRootImpl = mDecor != null ? mDecor.getViewRootImpl() : null;
        if (viewRootImpl != null) {
            return viewRootImpl;
        }
        throw new IllegalStateException("view not added");
    }

    @Override
    public void takeKeyEvents(boolean get) {
        mDecor.setFocusable(get);
    }

    @Override
    public void takeSurface(SurfaceHolder.Callback2 callback) {
        mTakeSurfaceCallback = callback;
    }

    @Override
    public void takeInputQueue(InputQueue.Callback callback) {
        mTakeInputQueueCallback = callback;
    }

    @Override
    public void setVolumeControlStream(int streamType) {
        mVolumeControlStreamType = streamType;
    }

    @Override
    public int getVolumeControlStream() {
        return mVolumeControlStreamType;
    }

    // Panels, feature drawables and the caption: none on Minato's windows, as no title or action bar shows them.

    @Override
    public void alwaysReadCloseOnTouchAttr() {
    }

    @Override
    public void openPanel(int featureId, KeyEvent event) {
    }

    @Override
    public void closePanel(int featureId) {
    }

    @Override
    public void togglePanel(int featureId, KeyEvent event) {
    }

    @Override
    public void invalidatePanelMenu(int featureId) {
    }

    @Override
    public boolean performPanelShortcut(int featureId, int keyCode, KeyEvent event, int flags) {
        return false;
    }

    @Override
    public boolean performPanelIdentifierAction(int featureId, int id, int flags) {
        return false;
    }

    @Override
    public void closeAllPanels() {
    }

    @Override
    public boolean performContextMenuIdentifierAction(int id, int flags) {
        return false;
    }

    @Override
    public boolean isShortcutKey(int keyCode, KeyEvent event) {
        return false;
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
    }

    @Override
    public void setFeatureDrawableResource(int featureId, int resId) {
    }

    @Override
    public void setFeatureDrawableUri(int featureId, Uri uri) {
    }

    @Override
    public void setFeatureDrawable(int featureId, Drawable drawable) {
    }

    @Override
    public void setFeatureDrawableAlpha(int featureId, int alpha) {
    }

    @Override
    public void setFeatureInt(int featureId, int value) {
    }

    @Override
    public void setChildDrawable(int featureId, Drawable drawable) {
    }

    @Override
    public void setChildInt(int featureId, int value) {
    }

    @Override
    protected void onActive() {
    }

    @Override
    public void setDecorCaptionShade(int decorCaptionShade) {
    }

    @Override
    public void setResizingCaptionDrawable(Drawable drawable) {
    }

    @Override
    public void onMultiWindowModeChanged() {
    }

    @Override
    public void onPictureInPictureModeChanged(boolean isInPictureInPictureMode) {
    }
}
