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
import android.content.res.Resources.Theme;
import android.content.res.TypedArray;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
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
import android.view.KeyCharacterMap;
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
import android.media.AudioManager;
import android.util.EventLog;
import android.view.ContextThemeWrapper;
import android.view.Menu;
import android.view.MenuItem;
import android.view.ViewConfiguration;
import android.view.ViewManager;
import android.view.ViewParent;
import com.android.internal.view.menu.MenuBuilder;
import com.android.internal.view.menu.MenuPresenter;
import com.android.internal.widget.DecorContentParent;
import java.util.List;

/** @hide */
@SuppressWarnings("deprecation") // Window's bar colors, flags and insets that it implements and keeps, as AOSP's.
public class PhoneWindow extends Window implements MenuBuilder.Callback {
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
    // The action bar and the panels (the platform action bar, step 3): AOSP's, for the options menu that the action bar
    // shows; a window without an action bar shows no panel.
    DecorContentParent mDecorContentParent;
    private PanelFeatureState[] mPanels;
    PanelFeatureState mPreparedPanel;
    int mPanelChordingKey;
    private boolean mClosingActionMenu;
    private boolean mInvalidatePanelMenuPosted;
    private int mInvalidatePanelMenuFeatures;
    private ActionMenuPresenterCallback mActionMenuPresenterCallback;
    private int mUiOptions;
    private int mIconRes;
    private int mLogoRes;
    private int mResourcesSetFlags;
    private static final int FLAG_RESOURCE_SET_ICON = 1 << 0;
    private static final int FLAG_RESOURCE_SET_LOGO = 1 << 1;
    private static final int FLAG_RESOURCE_SET_ICON_FALLBACK = 1 << 2;
    private boolean mIsStartingWindow;
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
            if (!mInvalidatePanelMenuPosted && mInvalidatePanelMenuFeatures != 0) {
                mDecor.postOnAnimation(mInvalidatePanelMenuRunnable);
            }
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
        } else if (a.getBoolean(R.styleable.Window_windowActionBar, false)) {
            // As AOSP's: no action bar without a title.
            requestFeature(FEATURE_ACTION_BAR);
        }
        if (a.getBoolean(R.styleable.Window_windowActionBarOverlay, false)) {
            requestFeature(FEATURE_ACTION_BAR_OVERLAY);
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
        // As AOSP's: a floating window's sizes, and every dialog's dim (ADR-0041 (b)).
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
        // As AOSP's: a title bar unless FEATURE_NO_TITLE, the dialog's for a floating window, the action bar's layout
        // for a window with the action bar feature.
        final int features = getLocalFeatures();
        final int layoutResource;
        if ((features & (1 << FEATURE_NO_TITLE)) == 0 && mIsFloating) {
            TypedValue res = new TypedValue();
            getContext().getTheme().resolveAttribute(R.attr.dialogTitleDecorLayout, res, true);
            layoutResource = res.resourceId;
        } else if ((features & (1 << FEATURE_NO_TITLE)) == 0 && (features & (1 << FEATURE_ACTION_BAR)) != 0) {
            layoutResource = a.getResourceId(R.styleable.Window_windowActionBarFullscreenDecorLayout,
                    R.layout.screen_action_bar);
        } else if ((features & (1 << FEATURE_NO_TITLE)) == 0) {
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
        final DecorContentParent decorContentParent = (DecorContentParent) decor.findViewById(
                R.id.decor_content_parent);
        if (decorContentParent != null) {
            // As AOSP's installDecor: the action bar's layout parent takes the window's callback, title, features, UI
            // options, icon and logo, and the options menu is made, deferred, for the bar.
            mDecorContentParent = decorContentParent;
            mDecorContentParent.setWindowCallback(getCallback());
            if (mDecorContentParent.getTitle() == null) {
                mDecorContentParent.setWindowTitle(mTitle);
            }
            final int localFeatures = getLocalFeatures();
            for (int i = 0; i < FEATURE_MAX; i++) {
                if ((localFeatures & (1 << i)) != 0) {
                    mDecorContentParent.initFeature(i);
                }
            }
            mDecorContentParent.setUiOptions(mUiOptions);
            if ((mResourcesSetFlags & FLAG_RESOURCE_SET_ICON) != 0
                    || (mIconRes != 0 && !mDecorContentParent.hasIcon())) {
                mDecorContentParent.setIcon(mIconRes);
            } else if ((mResourcesSetFlags & FLAG_RESOURCE_SET_ICON) == 0 && mIconRes == 0
                    && !mDecorContentParent.hasIcon()) {
                mDecorContentParent.setIcon(getContext().getPackageManager().getDefaultActivityIcon());
                mResourcesSetFlags |= FLAG_RESOURCE_SET_ICON_FALLBACK;
            }
            if ((mResourcesSetFlags & FLAG_RESOURCE_SET_LOGO) != 0
                    || (mLogoRes != 0 && !mDecorContentParent.hasLogo())) {
                mDecorContentParent.setLogo(mLogoRes);
            }
            // Panel menu invalidation is deferred, so that the app's onCreateOptionsMenu does not run in the middle of
            // its onCreate.
            final PanelFeatureState st = getPanelState(FEATURE_OPTIONS_PANEL, false);
            if (!isDestroyed() && (st == null || st.menu == null) && !mIsStartingWindow) {
                invalidatePanelMenu(FEATURE_ACTION_BAR);
            }
        } else {
            mTitleView = (TextView) findViewById(R.id.title);
            if (mTitleView != null) {
                mTitleView.setText(mTitle);
                if (mTitleColor != 0) {
                    mTitleView.setTextColor(mTitleColor);
                }
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
        } else if (mDecorContentParent != null) {
            mDecorContentParent.setWindowTitle(title);
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

    // The panels (AOSP's, from here to the end of the block).

    /**
     * Prepares the panel to either be opened or chorded. This creates the Menu
     * instance for the panel and populates it via the Activity callbacks.
     *
     * @param st The panel state to prepare.
     * @param event The event that triggered the preparing of the panel.
     * @return Whether the panel was prepared. If the panel should not be shown,
     *         returns false.
     */
    public final boolean preparePanel(PanelFeatureState st, KeyEvent event) {
        if (isDestroyed()) {
            return false;
        }

        // Already prepared (isPrepared will be reset to false later)
        if (st.isPrepared) {
            return true;
        }

        if ((mPreparedPanel != null) && (mPreparedPanel != st)) {
            // Another Panel is prepared and possibly open, so close it
            closePanel(mPreparedPanel, false);
        }

        final Callback cb = getCallback();

        if (cb != null) {
            st.createdPanelView = cb.onCreatePanelView(st.featureId);
        }

        final boolean isActionBarMenu =
                (st.featureId == FEATURE_OPTIONS_PANEL || st.featureId == FEATURE_ACTION_BAR);

        if (isActionBarMenu && mDecorContentParent != null) {
            // Enforce ordering guarantees around events so that the action bar never
            // dispatches menu-related events before the panel is prepared.
            mDecorContentParent.setMenuPrepared();
        }

        if (st.createdPanelView == null) {
            // Init the panel state's menu--return false if init failed
            if (st.menu == null || st.refreshMenuContent) {
                if (st.menu == null) {
                    if (!initializePanelMenu(st) || (st.menu == null)) {
                        return false;
                    }
                }

                if (isActionBarMenu && mDecorContentParent != null) {
                    if (mActionMenuPresenterCallback == null) {
                        mActionMenuPresenterCallback = new ActionMenuPresenterCallback();
                    }
                    mDecorContentParent.setMenu(st.menu, mActionMenuPresenterCallback);
                }

                // Call callback, and return if it doesn't want to display menu.

                // Creating the panel menu will involve a lot of manipulation;
                // don't dispatch change events to presenters until we're done.
                st.menu.stopDispatchingItemsChanged();
                if ((cb == null) || !cb.onCreatePanelMenu(st.featureId, st.menu)) {
                    // Ditch the menu created above
                    st.setMenu(null);

                    if (isActionBarMenu && mDecorContentParent != null) {
                        // Don't show it in the action bar either
                        mDecorContentParent.setMenu(null, mActionMenuPresenterCallback);
                    }

                    return false;
                }

                st.refreshMenuContent = false;
            }

            // Callback and return if the callback does not want to show the menu

            // Preparing the panel menu can involve a lot of manipulation;
            // don't dispatch change events to presenters until we're done.
            st.menu.stopDispatchingItemsChanged();

            // Restore action view state before we prepare. This gives apps
            // an opportunity to override frozen/restored state in onPrepare.
            if (st.frozenActionViewState != null) {
                st.menu.restoreActionViewStates(st.frozenActionViewState);
                st.frozenActionViewState = null;
            }

            if (!cb.onPreparePanel(st.featureId, st.createdPanelView, st.menu)) {
                if (isActionBarMenu && mDecorContentParent != null) {
                    // The app didn't want to show the menu for now but it still exists.
                    // Clear it out of the action bar.
                    mDecorContentParent.setMenu(null, mActionMenuPresenterCallback);
                }
                st.menu.startDispatchingItemsChanged();
                return false;
            }

            // Set the proper keymap
            KeyCharacterMap kmap = KeyCharacterMap.load(
                    event != null ? event.getDeviceId() : KeyCharacterMap.VIRTUAL_KEYBOARD);
            st.qwertyMode = kmap.getKeyboardType() != KeyCharacterMap.NUMERIC;
            st.menu.setQwertyMode(st.qwertyMode);
            st.menu.startDispatchingItemsChanged();
        }

        // Set other state
        st.isPrepared = true;
        st.isHandled = false;
        mPreparedPanel = st;

        return true;
    }

    @Override
    public final void openPanel(int featureId, KeyEvent event) {
        if (featureId == FEATURE_OPTIONS_PANEL && mDecorContentParent != null &&
                mDecorContentParent.canShowOverflowMenu() &&
                !ViewConfiguration.get(getContext()).hasPermanentMenuKey()) {
            mDecorContentParent.showOverflowMenu();
        } else {
            openPanel(getPanelState(featureId, true), event);
        }
    }

    @Override
    public final void closePanel(int featureId) {
        if (featureId == FEATURE_OPTIONS_PANEL && mDecorContentParent != null &&
                mDecorContentParent.canShowOverflowMenu() &&
                !ViewConfiguration.get(getContext()).hasPermanentMenuKey()) {
            mDecorContentParent.hideOverflowMenu();
        } else if (featureId == FEATURE_CONTEXT_MENU) {
            closeContextMenu();
        } else {
            closePanel(getPanelState(featureId, true), true);
        }
    }

    /**
     * Closes the given panel.
     *
     * @param st The panel to be closed.
     * @param doCallback Whether to notify the callback that the panel was
     *            closed. If the panel is in the process of re-opening or
     *            opening another panel (e.g., menu opening a sub menu), the
     *            callback should not happen and this variable should be false.
     *            In addition, this method internally will only perform the
     *            callback if the panel is open.
     */
    public final void closePanel(PanelFeatureState st, boolean doCallback) {
        // System.out.println("Close panel: isOpen=" + st.isOpen);
        if (doCallback && st.featureId == FEATURE_OPTIONS_PANEL &&
                mDecorContentParent != null && mDecorContentParent.isOverflowMenuShowing()) {
            checkCloseActionMenu(st.menu);
            return;
        }

        final ViewManager wm = getWindowManager();
        if ((wm != null) && st.isOpen) {
            if (st.decorView != null) {
                wm.removeView(st.decorView);
                // Log.v(TAG, "Removing main menu from window manager.");
            }

            if (doCallback) {
                callOnPanelClosed(st.featureId, st, null);
            }
        }

        st.isPrepared = false;
        st.isHandled = false;
        st.isOpen = false;

        // This view is no longer shown, so null it out
        st.shownPanelView = null;

        if (st.isInExpandedMode) {
            // Next time the menu opens, it should not be in expanded mode, so
            // force a refresh of the decor
            st.refreshDecorView = true;
            st.isInExpandedMode = false;
        }

        if (mPreparedPanel == st) {
            mPreparedPanel = null;
            mPanelChordingKey = 0;
        }
    }

    void checkCloseActionMenu(Menu menu) {
        if (mClosingActionMenu) {
            return;
        }

        mClosingActionMenu = true;
        mDecorContentParent.dismissPopups();
        Callback cb = getCallback();
        if (cb != null && !isDestroyed()) {
            cb.onPanelClosed(FEATURE_ACTION_BAR, menu);
        }
        mClosingActionMenu = false;
    }

    @Override
    public final void togglePanel(int featureId, KeyEvent event) {
        PanelFeatureState st = getPanelState(featureId, true);
        if (st.isOpen) {
            closePanel(st, true);
        } else {
            openPanel(st, event);
        }
    }

    @Override
    public void invalidatePanelMenu(int featureId) {
        mInvalidatePanelMenuFeatures |= 1 << featureId;

        if (!mInvalidatePanelMenuPosted && mDecor != null) {
            mDecor.postOnAnimation(mInvalidatePanelMenuRunnable);
            mInvalidatePanelMenuPosted = true;
        }
    }

    void doPendingInvalidatePanelMenu() {
        if (mInvalidatePanelMenuPosted) {
            mDecor.removeCallbacks(mInvalidatePanelMenuRunnable);
            mInvalidatePanelMenuRunnable.run();
        }
    }

    void doInvalidatePanelMenu(int featureId) {
        PanelFeatureState st = getPanelState(featureId, false);
        if (st == null) {
            return;
        }
        Bundle savedActionViewStates = null;
        if (st.menu != null) {
            savedActionViewStates = new Bundle();
            st.menu.saveActionViewStates(savedActionViewStates);
            if (savedActionViewStates.size() > 0) {
                st.frozenActionViewState = savedActionViewStates;
            }
            // This will be started again when the panel is prepared.
            st.menu.stopDispatchingItemsChanged();
            st.menu.clear();
        }
        st.refreshMenuContent = true;
        st.refreshDecorView = true;

        // Prepare the options panel if we have an action bar
        if ((featureId == FEATURE_ACTION_BAR || featureId == FEATURE_OPTIONS_PANEL)
                && mDecorContentParent != null) {
            st = getPanelState(Window.FEATURE_OPTIONS_PANEL, false);
            if (st != null) {
                st.isPrepared = false;
                preparePanel(st, null);
            }
        }
    }

    @Override
    public final void closeAllPanels() {
        final ViewManager wm = getWindowManager();
        if (wm == null) {
            return;
        }

        final PanelFeatureState[] panels = mPanels;
        final int N = panels != null ? panels.length : 0;
        for (int i = 0; i < N; i++) {
            final PanelFeatureState panel = panels[i];
            if (panel != null) {
                closePanel(panel, true);
            }
        }

        closeContextMenu();
    }

    @Override
    public boolean performPanelShortcut(int featureId, int keyCode, KeyEvent event, int flags) {
        return performPanelShortcut(getPanelState(featureId, false), keyCode, event, flags);
    }

    boolean performPanelShortcut(PanelFeatureState st, int keyCode, KeyEvent event,
            int flags) {
        if (event.isSystem() || (st == null)) {
            return false;
        }

        boolean handled = false;

        // Only try to perform menu shortcuts if preparePanel returned true (possible false
        // return value from application not wanting to show the menu).
        if ((st.isPrepared || preparePanel(st, event)) && st.menu != null) {
            // The menu is prepared now, perform the shortcut on it
            handled = st.menu.performShortcut(keyCode, event, flags);
        }

        if (handled) {
            // Mark as handled
            st.isHandled = true;

            // Only close down the menu if we don't have an action bar keeping it open.
            if ((flags & Menu.FLAG_PERFORM_NO_CLOSE) == 0 && mDecorContentParent == null) {
                closePanel(st, true);
            }
        }

        return handled;
    }

    @Override
    public boolean performPanelIdentifierAction(int featureId, int id, int flags) {

        PanelFeatureState st = getPanelState(featureId, true);
        if (!preparePanel(st, new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU))) {
            return false;
        }
        if (st.menu == null) {
            return false;
        }

        boolean res = st.menu.performIdentifierAction(id, flags);

        // Only close down the menu if we don't have an action bar keeping it open.
        if (mDecorContentParent == null) {
            closePanel(st, true);
        }

        return res;
    }

    public PanelFeatureState findMenuPanel(Menu menu) {
        final PanelFeatureState[] panels = mPanels;
        final int N = panels != null ? panels.length : 0;
        for (int i = 0; i < N; i++) {
            final PanelFeatureState panel = panels[i];
            if (panel != null && panel.menu == menu) {
                return panel;
            }
        }
        return null;
    }

    public boolean onMenuItemSelected(MenuBuilder menu, MenuItem item) {
        final Callback cb = getCallback();
        if (cb != null && !isDestroyed()) {
            final PanelFeatureState panel = findMenuPanel(menu.getRootMenu());
            if (panel != null) {
                return cb.onMenuItemSelected(panel.featureId, item);
            }
        }
        return false;
    }

    public void onMenuModeChange(MenuBuilder menu) {
        reopenMenu(true);
    }

    private void reopenMenu(boolean toggleMenuMode) {
        if (mDecorContentParent != null && mDecorContentParent.canShowOverflowMenu() &&
                (!ViewConfiguration.get(getContext()).hasPermanentMenuKey() ||
                        mDecorContentParent.isOverflowMenuShowPending())) {
            final Callback cb = getCallback();
            if (!mDecorContentParent.isOverflowMenuShowing() || !toggleMenuMode) {
                if (cb != null && !isDestroyed()) {
                    // If we have a menu invalidation pending, do it now.
                    if (mInvalidatePanelMenuPosted &&
                            (mInvalidatePanelMenuFeatures & (1 << FEATURE_OPTIONS_PANEL)) != 0) {
                        mDecor.removeCallbacks(mInvalidatePanelMenuRunnable);
                        mInvalidatePanelMenuRunnable.run();
                    }

                    final PanelFeatureState st = getPanelState(FEATURE_OPTIONS_PANEL, false);

                    // If we don't have a menu or we're waiting for a full content refresh,
                    // forget it. This is a lingering event that no longer matters.
                    if (st != null && st.menu != null && !st.refreshMenuContent &&
                            cb.onPreparePanel(FEATURE_OPTIONS_PANEL, st.createdPanelView, st.menu)) {
                        cb.onMenuOpened(FEATURE_ACTION_BAR, st.menu);
                        mDecorContentParent.showOverflowMenu();
                    }
                }
            } else {
                mDecorContentParent.hideOverflowMenu();
                final PanelFeatureState st = getPanelState(FEATURE_OPTIONS_PANEL, false);
                if (st != null && cb != null && !isDestroyed()) {
                    cb.onPanelClosed(FEATURE_ACTION_BAR, st.menu);
                }
            }
            return;
        }

        PanelFeatureState st = getPanelState(FEATURE_OPTIONS_PANEL, false);

        if (st == null) {
            return;
        }

        // Save the future expanded mode state since closePanel will reset it
        boolean newExpandedMode = toggleMenuMode ? !st.isInExpandedMode : st.isInExpandedMode;

        st.refreshDecorView = true;
        closePanel(st, false);

        // Set the expanded mode state
        st.isInExpandedMode = newExpandedMode;

        openPanel(st, null);
    }

    /**
     * Initializes the menu associated with the given panel feature state. You
     * must at the very least set PanelFeatureState.menu to the Menu to be
     * associated with the given panel state. The default implementation creates
     * a new menu for the panel state.
     *
     * @param st The panel whose menu is being initialized.
     * @return Whether the initialization was successful.
     */
    protected boolean initializePanelMenu(final PanelFeatureState st) {
        Context context = getContext();

        // If we have an action bar, initialize the menu with the right theme.
        if ((st.featureId == FEATURE_OPTIONS_PANEL || st.featureId == FEATURE_ACTION_BAR) &&
                mDecorContentParent != null) {
            final TypedValue outValue = new TypedValue();
            final Theme baseTheme = context.getTheme();
            baseTheme.resolveAttribute(R.attr.actionBarTheme, outValue, true);

            Theme widgetTheme = null;
            if (outValue.resourceId != 0) {
                widgetTheme = context.getResources().newTheme();
                widgetTheme.setTo(baseTheme);
                widgetTheme.applyStyle(outValue.resourceId, true);
                widgetTheme.resolveAttribute(
                        R.attr.actionBarWidgetTheme, outValue, true);
            } else {
                baseTheme.resolveAttribute(
                        R.attr.actionBarWidgetTheme, outValue, true);
            }

            if (outValue.resourceId != 0) {
                if (widgetTheme == null) {
                    widgetTheme = context.getResources().newTheme();
                    widgetTheme.setTo(baseTheme);
                }
                widgetTheme.applyStyle(outValue.resourceId, true);
            }

            if (widgetTheme != null) {
                context = new ContextThemeWrapper(context, 0);
                context.getTheme().setTo(widgetTheme);
            }
        }

        final MenuBuilder menu = new MenuBuilder(context);
        menu.setCallback(this);
        st.setMenu(menu);

        return true;
    }

    /**
     * Gets a panel's state based on its feature ID.
     *
     * @param featureId The feature ID of the panel.
     * @param required Whether the panel is required (if it is required and it
     *            isn't in our features, this throws an exception).
     * @return The panel state.
     */
    PanelFeatureState getPanelState(int featureId, boolean required) {
        return getPanelState(featureId, required, null);
    }

    /**
     * Gets a panel's state based on its feature ID.
     *
     * @param featureId The feature ID of the panel.
     * @param required Whether the panel is required (if it is required and it
     *            isn't in our features, this throws an exception).
     * @param convertPanelState Optional: If the panel state does not exist, use
     *            this as the panel state.
     * @return The panel state.
     */
    private PanelFeatureState getPanelState(int featureId, boolean required,
            PanelFeatureState convertPanelState) {
        if ((getFeatures() & (1 << featureId)) == 0) {
            if (!required) {
                return null;
            }
            throw new RuntimeException("The feature has not been requested");
        }

        PanelFeatureState[] ar;
        if ((ar = mPanels) == null || ar.length <= featureId) {
            PanelFeatureState[] nar = new PanelFeatureState[featureId + 1];
            if (ar != null) {
                System.arraycopy(ar, 0, nar, 0, ar.length);
            }
            mPanels = ar = nar;
        }

        PanelFeatureState st = ar[featureId];
        if (st == null) {
            ar[featureId] = st = (convertPanelState != null)
                    ? convertPanelState
                    : new PanelFeatureState(featureId);
        }
        return st;
    }

    /**
     * Helper method for calling the {@link Callback#onPanelClosed(int, Menu)}
     * callback. This method will grab whatever extra state is needed for the
     * callback that isn't given in the parameters. If the panel is not open,
     * this will not perform the callback.
     *
     * @param featureId Feature ID of the panel that was closed. Must be given.
     * @param panel Panel that was closed. Optional but useful if there is no
     *            menu given.
     * @param menu The menu that was closed. Optional, but give if you have.
     */
    private void callOnPanelClosed(int featureId, PanelFeatureState panel, Menu menu) {
        final Callback cb = getCallback();
        if (cb == null)
            return;

        // Try to get a menu
        if (menu == null) {
            // Need a panel to grab the menu, so try to get that
            if (panel == null) {
                if ((featureId >= 0) && (featureId < mPanels.length)) {
                    panel = mPanels[featureId];
                }
            }

            if (panel != null) {
                // menu still may be null, which is okay--we tried our best
                menu = panel.menu;
            }
        }

        // If the panel is not open, do not callback
        if ((panel != null) && (!panel.isOpen))
            return;

        if (!isDestroyed()) {
            cb.onPanelClosed(featureId, menu);
        }
    }

    private final class ActionMenuPresenterCallback implements MenuPresenter.Callback {
        @Override
        public boolean onOpenSubMenu(MenuBuilder subMenu) {
            Callback cb = getCallback();
            if (cb != null) {
                cb.onMenuOpened(FEATURE_ACTION_BAR, subMenu);
                return true;
            }
            return false;
        }

        @Override
        public void onCloseMenu(MenuBuilder menu, boolean allMenusAreClosing) {
            checkCloseActionMenu(menu);
        }
    }

    /**
     * AOSP's PanelFeatureState for the panels the action bar shows: the options menu's state. The panels that are a
     * window of their own (popup list and icon menus), their presenters and their saved state are left out.
     */
    static final class PanelFeatureState {
        /** The panel's feature, one of Window's FEATURE_OPTIONS_PANEL and FEATURE_ACTION_BAR. */
        int featureId;
        View createdPanelView;
        View shownPanelView;
        MenuBuilder menu;
        boolean isCompact;
        boolean isPrepared;
        boolean isHandled;
        boolean isOpen;
        boolean isInExpandedMode;
        public boolean qwertyMode;
        boolean refreshDecorView;
        boolean refreshMenuContent;
        Bundle frozenActionViewState;
        DecorView decorView;

        PanelFeatureState(int featureId) {
            this.featureId = featureId;
            refreshDecorView = false;
        }

        public boolean isInListMode() {
            return isInExpandedMode || isCompact;
        }

        public boolean hasPanelItems() {
            return shownPanelView != null && (createdPanelView != null || ((ViewGroup) shownPanelView).getChildCount() > 0);
        }

        public void clearMenuPresenters() {
        }

        void setStyle(Context context) {
        }

        void setMenu(MenuBuilder menu) {
            this.menu = menu;
        }
    }

    private final Runnable mInvalidatePanelMenuRunnable = new Runnable() {
        @Override public void run() {
            for (int i = 0; i <= FEATURE_MAX; i++) {
                if ((mInvalidatePanelMenuFeatures & 1 << i) != 0) {
                    doInvalidatePanelMenu(i);
                }
            }
            mInvalidatePanelMenuPosted = false;
            mInvalidatePanelMenuFeatures = 0;
        }
    };

    /**
     * Called when the panel key is pushed down.
     * @param featureId The feature ID of the relevant panel (defaults to FEATURE_OPTIONS_PANEL}.
     * @param event The key event.
     * @return Whether the key was handled.
     */
    public final boolean onKeyDownPanel(int featureId, KeyEvent event) {
        final int keyCode = event.getKeyCode();

        if (event.getRepeatCount() == 0) {
            // The panel key was pushed, so set the chording key
            mPanelChordingKey = keyCode;

            PanelFeatureState st = getPanelState(featureId, false);
            if (st != null && !st.isOpen) {
                return preparePanel(st, event);
            }
        }

        return false;
    }

    /**
     * Called when the panel key is released.
     * @param featureId The feature ID of the relevant panel (defaults to FEATURE_OPTIONS_PANEL}.
     * @param event The key event.
     */
    public final void onKeyUpPanel(int featureId, KeyEvent event) {
        // The panel key was released, so clear the chording key
        if (mPanelChordingKey != 0) {
            mPanelChordingKey = 0;

            final PanelFeatureState st = getPanelState(featureId, false);

            if (event.isCanceled() || (mDecor != null && mDecor.mPrimaryActionMode != null) ||
                    (st == null)) {
                return;
            }

            boolean playSoundEffect = false;
            if (featureId == FEATURE_OPTIONS_PANEL && mDecorContentParent != null &&
                    mDecorContentParent.canShowOverflowMenu() &&
                    !ViewConfiguration.get(getContext()).hasPermanentMenuKey()) {
                if (!mDecorContentParent.isOverflowMenuShowing()) {
                    if (!isDestroyed() && preparePanel(st, event)) {
                        playSoundEffect = mDecorContentParent.showOverflowMenu();
                    }
                } else {
                    playSoundEffect = mDecorContentParent.hideOverflowMenu();
                }
            } else {
                if (st.isOpen || st.isHandled) {

                    // Play the sound effect if the user closed an open menu (and not if
                    // they just released a menu shortcut)
                    playSoundEffect = st.isOpen;

                    // Close menu
                    closePanel(st, true);

                } else if (st.isPrepared) {
                    boolean show = true;
                    if (st.refreshMenuContent) {
                        // Something may have invalidated the menu since we prepared it.
                        // Re-prepare it to refresh.
                        st.isPrepared = false;
                        show = preparePanel(st, event);
                    }

                    if (show) {
                        // Write 'menu opened' to event log
                        EventLog.writeEvent(50001, 0);

                        // Show menu
                        openPanel(st, event);

                        playSoundEffect = true;
                    }
                }
            }

            if (playSoundEffect) {
                AudioManager audioManager = (AudioManager) getContext().getSystemService(
                        Context.AUDIO_SERVICE);
                if (audioManager != null) {
                    audioManager.playSoundEffect(AudioManager.FX_KEY_CLICK);
                } else {
                    Log.w(TAG, "Couldn't get audio manager");
                }
            }
        }
    }

    /**
     * As AOSP's onKeyDown for the keys of a window's own: the MENU key, which pushes the options panel's key down. The
     * volume, media, camera and search keys AOSP's handles there come with ADR-0041 a)'s input.
     */
    protected boolean onKeyDown(int featureId, int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_MENU) {
            onKeyDownPanel((featureId < 0) ? FEATURE_OPTIONS_PANEL : featureId, event);
            return true;
        }
        return false;
    }

    /** As AOSP's onKeyUp for the MENU key, which opens or closes the options panel (the action bar's overflow). */
    protected boolean onKeyUp(int featureId, int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_MENU) {
            onKeyUpPanel(featureId < 0 ? FEATURE_OPTIONS_PANEL : featureId, event);
            return true;
        }
        return false;
    }

    private void closeContextMenu() {
    }

    /**
     * AOSP's openPanel for a panel that is a window of its own (a popup list or icon menu), which Minato does not make:
     * only the action bar shows the options menu, through the overflow.
     */
    private void openPanel(final PanelFeatureState st, KeyEvent event) {
    }

    @Override
    public void setUiOptions(int uiOptions) {
        mUiOptions = uiOptions;
    }

    @Override
    public void setUiOptions(int uiOptions, int mask) {
        mUiOptions = (mUiOptions & ~mask) | (uiOptions & mask);
    }

    /** As AOSP's: the window's icon and logo, for the action bar. */
    @Override
    public void setIcon(int resId) {
        mIconRes = resId;
        mResourcesSetFlags |= FLAG_RESOURCE_SET_ICON;
        mResourcesSetFlags &= ~FLAG_RESOURCE_SET_ICON_FALLBACK;
        if (mDecorContentParent != null) {
            mDecorContentParent.setIcon(resId);
        }
    }

    @Override
    public void setDefaultIcon(int resId) {
        if ((mResourcesSetFlags & FLAG_RESOURCE_SET_ICON) != 0) {
            return;
        }
        mIconRes = resId;
        if (mDecorContentParent != null && (!mDecorContentParent.hasIcon()
                || (mResourcesSetFlags & FLAG_RESOURCE_SET_ICON_FALLBACK) != 0)) {
            if (resId != 0) {
                mDecorContentParent.setIcon(resId);
                mResourcesSetFlags &= ~FLAG_RESOURCE_SET_ICON_FALLBACK;
            } else {
                mDecorContentParent.setIcon(getContext().getPackageManager().getDefaultActivityIcon());
                mResourcesSetFlags |= FLAG_RESOURCE_SET_ICON_FALLBACK;
            }
        }
    }

    @Override
    public void setLogo(int resId) {
        mLogoRes = resId;
        mResourcesSetFlags |= FLAG_RESOURCE_SET_LOGO;
        if (mDecorContentParent != null) {
            mDecorContentParent.setLogo(resId);
        }
    }

    @Override
    public void setDefaultLogo(int resId) {
        if ((mResourcesSetFlags & FLAG_RESOURCE_SET_LOGO) != 0) {
            return;
        }
        mLogoRes = resId;
        if (mDecorContentParent != null && !mDecorContentParent.hasLogo()) {
            mDecorContentParent.setLogo(resId);
        }
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
