/*
 * Our ViewRootImpl, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3): the root of one window, the parent
 * of its decor view. AOSP's (13,254 lines) asks system_server's window manager for its frame, surface and input; ours
 * lays out locally:
 *   - setView attaches the window as AOSP's does after addToDisplayAsUser: the view gets this as its parent, the
 *     window is in touch mode, as a touch screen's starts, and a traversal is scheduled;
 *   - a traversal runs as AOSP's on Choreographer's CALLBACK_TRAVERSAL behind a sync barrier. The first attaches the
 *     view tree, dispatches the window's insets and measures the tree at the display's size, which the window
 *     covers whole (ADR-0038); each traversal lays out what requested it, dispatches the global layout and the
 *     pre-draw, and draws nothing until M4 step 10. After the first, the window gains focus, as a new activity's
 *     window does once system_server shows it;
 *   - the insets are none until ADR-0041 decision 8, so a window that fits the system windows keeps its whole size,
 *     as an edge-to-edge window does on Android 15;
 *   - the back dispatcher is AOSP's, local: without a window session it keeps the callbacks and tells no one
 *     (ADR-0041 c) wires the back gesture);
 *   - the members that View, ViewGroup, Window and Activity reach answer as AOSP's does for a window without input,
 *     drawing, accessibility or IME focus, which come with ADR-0041 a)'s rest.
 */
package android.view;

import android.animation.LayoutTransition;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Matrix;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.Region;
import android.graphics.RenderNode;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.View.AttachInfo;
import android.view.Window.OnContentApplyWindowInsetsListener;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.window.ActivityWindowInfo;
import android.window.WindowOnBackInvokedDispatcher;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

@SuppressWarnings("deprecation") // ViewParent's invalidation and Display.getRealSize, which it keeps, as AOSP's.
public final class ViewRootImpl implements ViewParent, View.AttachInfo.Callbacks, AttachedSurfaceControl {
    private static final String TAG = "ViewRootImpl";

    /** As AOSP's: View asks whether frame-rate votes come from intermittent updates. */
    public static final int INTERMITTENT_STATE_INTERMITTENT = 0;
    public static final int INTERMITTENT_STATE_NOT_INTERMITTENT = 1;

    private static final int MSG_WINDOW_FOCUS_CHANGED = 1;
    private static final int MSG_DIE = 2;
    private static final int MSG_INVALIDATE = 3;
    private static final int MSG_INVALIDATE_RECT = 4;

    /** Called with the activity's configuration; AOSP's ActivityThread gives it every window of an activity. */
    public interface ActivityConfigCallback {
        default void onConfigurationChanged(Configuration overrideConfig, int newDisplayId) {
        }

        default void onConfigurationChanged(Configuration overrideConfig, int newDisplayId,
                ActivityWindowInfo activityWindowInfo) {
            onConfigurationChanged(overrideConfig, newDisplayId);
        }

        void requestCompatCameraControl(boolean showControl, boolean transformationApplied,
                android.app.ICompatCameraControlCallback callback);
    }

    final Thread mThread;
    public final WindowManager.LayoutParams mWindowAttributes = new WindowManager.LayoutParams();
    final Choreographer mChoreographer;
    /** As AOSP's: set by ViewGroup while a layout transition runs, so that empty invalidations still count. */
    public boolean mIsAnimating;
    /** The input frame's statistics, which View reads only while it handles a pointer event (ADR-0041 a)'s input). */
    protected final ViewFrameInfo mViewFrameInfo = null;
    /** The window's surface: invalid until ADR-0041 a)'s surfaces give it M3's layer. */
    public final Surface mSurface = new Surface();
    int mCurScrollY;

    final AttachInfo mAttachInfo;
    final ViewRootHandler mHandler = new ViewRootHandler();
    private final Context mContext;
    private final Display mDisplay;
    private final W mWindow = new W();
    private final WindowOnBackInvokedDispatcher mOnBackInvokedDispatcher;
    private final Rect mTempRect = new Rect();
    private final ImeFocusController mImeFocusController = new ImeFocusController();
    private final HandwritingInitiator mHandwritingInitiator = new HandwritingInitiator();
    private final Runnable mTraversalRunnable = this::doTraversal;
    private View mView;
    private ActivityConfigCallback mActivityConfigCallback;
    private List<Rect> mRootGestureExclusionRects = Collections.emptyList();
    private boolean mAdded;
    private boolean mRemoved;
    private boolean mStopped;
    private boolean mFirst = true;
    private boolean mLayoutRequested;
    private boolean mInLayout;
    private boolean mTraversalScheduled;
    private int mTraversalBarrier;
    private boolean mApplyInsetsRequested;
    private boolean mUpcomingWindowFocus;
    private boolean mWindowFocusChanged;
    private int mWidth = -1;
    private int mHeight = -1;
    private WindowInsets mLastWindowInsets;

    public ViewRootImpl(Context context, Display display) {
        mContext = context;
        mDisplay = display;
        mThread = Thread.currentThread();
        mAttachInfo = new AttachInfo(null, mWindow, display, this, mHandler, this, context);
        mChoreographer = Choreographer.getInstance();
        mOnBackInvokedDispatcher = new WindowOnBackInvokedDispatcher(context, Looper.myLooper());
    }

    /** The window's binder, as AttachInfo's token. */
    static final class W extends IWindow.Stub {
    }

    final class ViewRootHandler extends Handler {
        ViewRootHandler() {
            super(Looper.myLooper());
        }

        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
                case MSG_WINDOW_FOCUS_CHANGED:
                    handleWindowFocusChanged();
                    break;
                case MSG_DIE:
                    doDie();
                    break;
                case MSG_INVALIDATE:
                    ((View) msg.obj).invalidate();
                    break;
                case MSG_INVALIDATE_RECT:
                    final AttachInfo.InvalidateInfo info = (AttachInfo.InvalidateInfo) msg.obj;
                    info.target.invalidate(info.left, info.top, info.right, info.bottom);
                    info.recycle();
                    break;
                default:
                    break;
            }
        }
    }

    /** As AOSP's after the window manager added the window: the view's parent, touch mode, the first traversal. */
    public void setView(View view, WindowManager.LayoutParams attrs, View panelParentView, int userId) {
        synchronized (this) {
            if (mView != null) {
                return;
            }
            mView = view;
            mWindowAttributes.copyFrom(attrs);
            mAttachInfo.mRootView = view;
            mAttachInfo.mDisplayState = mDisplay.getState();
            mAttachInfo.mApplicationScale = 1.0f;
            mAttachInfo.mInTouchMode = true;
            mAdded = true;
            requestLayout();
            view.assignParent(this);
        }
    }

    public View getView() {
        return mView;
    }

    /** As AOSP's: new attributes take effect at the next traversal. */
    void setLayoutParams(WindowManager.LayoutParams attrs, boolean newView) {
        synchronized (this) {
            mWindowAttributes.copyFrom(attrs);
            mLayoutRequested = true;
            scheduleTraversals();
        }
    }

    void scheduleTraversals() {
        if (!mTraversalScheduled) {
            mTraversalScheduled = true;
            mTraversalBarrier = mHandler.getLooper().getQueue().postSyncBarrier();
            mChoreographer.postCallback(Choreographer.CALLBACK_TRAVERSAL, mTraversalRunnable, null);
        }
    }

    void unscheduleTraversals() {
        if (mTraversalScheduled) {
            mTraversalScheduled = false;
            mHandler.getLooper().getQueue().removeSyncBarrier(mTraversalBarrier);
            mChoreographer.removeCallbacks(Choreographer.CALLBACK_TRAVERSAL, mTraversalRunnable, null);
        }
    }

    void doTraversal() {
        if (mTraversalScheduled) {
            mTraversalScheduled = false;
            mHandler.getLooper().getQueue().removeSyncBarrier(mTraversalBarrier);
            performTraversals();
        }
    }

    private void performTraversals() {
        final View host = mView;
        if (host == null || !mAdded) {
            return;
        }
        final boolean first = mFirst;
        final int viewVisibility = host.getVisibility();
        Point size = new Point();
        mDisplay.getRealSize(size);
        if (first) {
            mAttachInfo.mUse32BitDrawingCache = true;
            mAttachInfo.mWindowVisibility = viewVisibility;
            mAttachInfo.mRecomputeGlobalAttributes = false;
            host.setLayoutDirection(mContext.getResources().getConfiguration().getLayoutDirection());
            host.dispatchAttachedToWindow(mAttachInfo, 0);
            mAttachInfo.mTreeObserver.dispatchOnWindowAttachedChange(true);
            dispatchApplyInsets(host);
            mLayoutRequested = true;
        } else if (mApplyInsetsRequested) {
            dispatchApplyInsets(host);
            mLayoutRequested = true;
        }
        if (mAttachInfo.mWindowVisibility != viewVisibility) {
            mAttachInfo.mWindowVisibility = viewVisibility;
            host.dispatchWindowVisibilityChanged(viewVisibility);
        }
        boolean triggerGlobalLayoutListener = false;
        if (mLayoutRequested) {
            mLayoutRequested = false;
            final WindowManager.LayoutParams lp = mWindowAttributes;
            host.measure(getRootMeasureSpec(size.x, lp.width), getRootMeasureSpec(size.y, lp.height));
            mWidth = host.getMeasuredWidth();
            mHeight = host.getMeasuredHeight();
            mInLayout = true;
            host.layout(0, 0, mWidth, mHeight);
            mInLayout = false;
            triggerGlobalLayoutListener = true;
        }
        if (triggerGlobalLayoutListener || mAttachInfo.mRecomputeGlobalAttributes) {
            mAttachInfo.mRecomputeGlobalAttributes = false;
            mAttachInfo.mTreeObserver.dispatchOnGlobalLayout();
        }
        if (first) {
            // As AOSP's first traversal in touch mode: a focused group that wants its children first gives way.
            View focused = host.findFocus();
            if (focused instanceof ViewGroup group
                    && group.getDescendantFocusability() == ViewGroup.FOCUS_AFTER_DESCENDANTS) {
                focused.restoreDefaultFocus();
            }
        }
        mFirst = false;
        boolean cancelDraw = mAttachInfo.mTreeObserver.dispatchOnPreDraw() || viewVisibility != View.VISIBLE;
        if (cancelDraw && viewVisibility == View.VISIBLE) {
            // As AOSP's: a pre-draw listener that cancels the draw gets another traversal at the next frame.
            scheduleTraversals();
        }
        if (first) {
            windowFocusChanged(true);
        }
    }

    private static int getRootMeasureSpec(int windowSize, int rootDimension) {
        switch (rootDimension) {
            case ViewGroup.LayoutParams.MATCH_PARENT:
                return View.MeasureSpec.makeMeasureSpec(windowSize, View.MeasureSpec.EXACTLY);
            case ViewGroup.LayoutParams.WRAP_CONTENT:
                return View.MeasureSpec.makeMeasureSpec(windowSize, View.MeasureSpec.AT_MOST);
            default:
                return View.MeasureSpec.makeMeasureSpec(rootDimension, View.MeasureSpec.EXACTLY);
        }
    }

    /** As AOSP's, with the window's insets, none yet (ADR-0041 decision 8). */
    public void dispatchApplyInsets(View host) {
        mApplyInsetsRequested = false;
        host.dispatchApplyWindowInsets(getWindowInsets(true));
    }

    WindowInsets getWindowInsets(boolean forceConstruct) {
        if (mLastWindowInsets == null || forceConstruct) {
            mLastWindowInsets = new WindowInsets.Builder().build();
        }
        return mLastWindowInsets;
    }

    /** What system_server reports as the window gains or loses focus, posted as AOSP's is. */
    public void windowFocusChanged(boolean hasFocus) {
        synchronized (this) {
            mWindowFocusChanged = true;
            mUpcomingWindowFocus = hasFocus;
        }
        mHandler.sendEmptyMessage(MSG_WINDOW_FOCUS_CHANGED);
    }

    private void handleWindowFocusChanged() {
        final boolean hasWindowFocus;
        synchronized (this) {
            if (!mWindowFocusChanged) {
                return;
            }
            mWindowFocusChanged = false;
            hasWindowFocus = mUpcomingWindowFocus;
        }
        if (mAdded && mView != null) {
            mAttachInfo.mHasWindowFocus = hasWindowFocus;
            mAttachInfo.mKeyDispatchState.reset();
            mView.dispatchWindowFocusChanged(hasWindowFocus);
            mAttachInfo.mTreeObserver.dispatchOnWindowFocusChange(hasWindowFocus);
            if (hasWindowFocus) {
                mWindowAttributes.softInputMode &= ~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
                ((WindowManager.LayoutParams) mView.getLayoutParams()).softInputMode &=
                        ~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
            }
        }
    }

    /** As AOSP's: the window goes now, or at its next message while a traversal runs. */
    boolean die(boolean immediate) {
        if (immediate) {
            doDie();
            return false;
        }
        mHandler.sendEmptyMessage(MSG_DIE);
        return true;
    }

    void doDie() {
        checkThread();
        synchronized (this) {
            if (mRemoved) {
                return;
            }
            mRemoved = true;
            if (mAdded && mView != null) {
                mAttachInfo.mTreeObserver.dispatchOnWindowAttachedChange(false);
                mView.dispatchDetachedFromWindow();
                mView.assignParent(null);
                mView = null;
                mAttachInfo.mRootView = null;
            }
            mAdded = false;
            unscheduleTraversals();
        }
        WindowManagerGlobal.getInstance().doRemoveView(this);
    }

    /** As AOSP's: what a view's change from another thread than its window's throws. */
    public static final class CalledFromWrongThreadException extends android.util.AndroidRuntimeException {
        private static final long serialVersionUID = 1L;

        public CalledFromWrongThreadException(String msg) {
            super(msg);
        }
    }

    void checkThread() {
        if (mThread != Thread.currentThread()) {
            throw new CalledFromWrongThreadException(
                    "Only the original thread that created a view hierarchy can touch its views.");
        }
    }

    // ViewParent.

    @Override
    public void requestLayout() {
        if (!mInLayout) {
            checkThread();
            mLayoutRequested = true;
            scheduleTraversals();
        }
    }

    @Override
    public boolean isLayoutRequested() {
        return mLayoutRequested;
    }

    @Override
    public void requestTransparentRegion(View child) {
        checkThread();
        if (mView == child) {
            requestLayout();
        }
    }

    @Override
    public void invalidateChild(View child, Rect dirty) {
        invalidateChildInParent(null, dirty);
    }

    /** As AOSP's: an invalidation asks for a traversal, which draws once M4 step 10 does. */
    @Override
    public ViewParent invalidateChildInParent(int[] location, Rect dirty) {
        checkThread();
        if (dirty == null || !dirty.isEmpty() || mIsAnimating) {
            scheduleTraversals();
        }
        return null;
    }

    /** The parent of a window's root: none. */
    @Override
    public ViewParent getParent() {
        return null;
    }

    @Override
    public void requestChildFocus(View child, View focused) {
        checkThread();
        scheduleTraversals();
    }

    @Override
    public void recomputeViewAttributes(View child) {
        checkThread();
        if (mView == child) {
            mAttachInfo.mRecomputeGlobalAttributes = true;
            scheduleTraversals();
        }
    }

    @Override
    public void clearChildFocus(View child) {
        checkThread();
        scheduleTraversals();
    }

    @Override
    public boolean getChildVisibleRect(View child, Rect r, Point offset) {
        if (child != mView) {
            throw new RuntimeException("child is not mine, honest!");
        }
        return r.intersect(0, 0, mWidth, mHeight);
    }

    @Override
    public View focusSearch(View focused, int direction) {
        checkThread();
        if (!(mView instanceof ViewGroup)) {
            return null;
        }
        return FocusFinder.getInstance().findNextFocus((ViewGroup) mView, focused, direction);
    }

    @Override
    public View keyboardNavigationClusterSearch(View currentCluster, int direction) {
        checkThread();
        return FocusFinder.getInstance().findNextKeyboardNavigationCluster(mView, currentCluster, direction);
    }

    @Override
    public void bringChildToFront(View child) {
    }

    @Override
    public void focusableViewAvailable(View v) {
        checkThread();
        if (mView != null && mView.hasFocus()) {
            View focused = mView.findFocus();
            if (focused instanceof ViewGroup group
                    && group.getDescendantFocusability() == ViewGroup.FOCUS_AFTER_DESCENDANTS
                    && isViewDescendantOf(v, focused)) {
                v.requestFocus();
            }
        }
    }

    @Override
    public boolean showContextMenuForChild(View originalView) {
        return false;
    }

    @Override
    public boolean showContextMenuForChild(View originalView, float x, float y) {
        return false;
    }

    @Override
    public void createContextMenu(ContextMenu menu) {
    }

    @Override
    public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback) {
        return null;
    }

    @Override
    public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback, int type) {
        return null;
    }

    @Override
    public void childDrawableStateChanged(View child) {
    }

    @Override
    public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {
    }

    /** No window scrolls on Minato yet, so no rectangle needs one. */
    @Override
    public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) {
        return false;
    }

    /** Accessibility is off on Minato (ADR-0041 decision 2), so no event goes anywhere. */
    @Override
    public boolean requestSendAccessibilityEvent(View child, AccessibilityEvent event) {
        return false;
    }

    @Override
    public void childHasTransientStateChanged(View child, boolean hasTransientState) {
    }

    @Override
    public void requestFitSystemWindows() {
        checkThread();
        mApplyInsetsRequested = true;
        scheduleTraversals();
    }

    @Override
    public ViewParent getParentForAccessibility() {
        return null;
    }

    @Override
    public void notifySubtreeAccessibilityStateChanged(View child, View source, int changeType) {
    }

    @Override
    public boolean canResolveLayoutDirection() {
        return true;
    }

    @Override
    public boolean isLayoutDirectionResolved() {
        return true;
    }

    @Override
    public int getLayoutDirection() {
        return View.LAYOUT_DIRECTION_RESOLVED_DEFAULT;
    }

    @Override
    public boolean canResolveTextDirection() {
        return true;
    }

    @Override
    public boolean isTextDirectionResolved() {
        return true;
    }

    @Override
    public int getTextDirection() {
        return View.TEXT_DIRECTION_RESOLVED_DEFAULT;
    }

    @Override
    public boolean canResolveTextAlignment() {
        return true;
    }

    @Override
    public boolean isTextAlignmentResolved() {
        return true;
    }

    @Override
    public int getTextAlignment() {
        return View.TEXT_ALIGNMENT_RESOLVED_DEFAULT;
    }

    @Override
    public boolean onStartNestedScroll(View child, View target, int nestedScrollAxes) {
        return false;
    }

    @Override
    public void onNestedScrollAccepted(View child, View target, int nestedScrollAxes) {
    }

    @Override
    public void onStopNestedScroll(View target) {
    }

    @Override
    public void onNestedScroll(View target, int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed) {
    }

    @Override
    public void onNestedPreScroll(View target, int dx, int dy, int[] consumed) {
    }

    @Override
    public boolean onNestedFling(View target, float velocityX, float velocityY, boolean consumed) {
        return false;
    }

    @Override
    public boolean onNestedPreFling(View target, float velocityX, float velocityY) {
        return false;
    }

    @Override
    public boolean onNestedPrePerformAccessibilityAction(View target, int action, Bundle args) {
        return false;
    }

    // View.AttachInfo.Callbacks: no sound effects or haptics yet.

    @Override
    public void playSoundEffect(int effectId) {
    }

    @Override
    public boolean performHapticFeedback(int effectId, boolean always, boolean fromIme) {
        return false;
    }

    // AttachedSurfaceControl: the window's layer comes with ADR-0041 a)'s surfaces.

    @Override
    public SurfaceControl.Transaction buildReparentTransaction(SurfaceControl child) {
        return null;
    }

    @Override
    public boolean applyTransactionOnDraw(SurfaceControl.Transaction t) {
        return false;
    }

    public SurfaceControl getSurfaceControl() {
        return null;
    }

    // What PhoneWindow, Activity, View and ViewGroup call.

    public void setActivityConfigCallback(ActivityConfigCallback callback) {
        mActivityConfigCallback = callback;
    }

    /** PhoneWindow's applier of the content's insets, which a framework-optional fitsSystemWindows view uses. */
    public void setOnContentApplyWindowInsetsListener(OnContentApplyWindowInsetsListener listener) {
        mAttachInfo.mContentOnApplyWindowInsetsListener = listener;
        if (!mFirst) {
            requestFitSystemWindows();
        }
    }

    public WindowOnBackInvokedDispatcher getOnBackInvokedDispatcher() {
        return mOnBackInvokedDispatcher;
    }

    public void setRootSystemGestureExclusionRects(List<Rect> rects) {
        mRootGestureExclusionRects = new ArrayList<>(rects);
    }

    public List<Rect> getRootSystemGestureExclusionRects() {
        return mRootGestureExclusionRects;
    }

    /** As AOSP's: a stopped activity's window, which traverses again once its activity restarts. */
    void setWindowStopped(boolean stopped) {
        checkThread();
        if (mStopped != stopped) {
            mStopped = stopped;
            if (!stopped) {
                scheduleTraversals();
            }
        }
    }

    /** The window's opacity, which only the GPU's canvas uses (M4 step 10). */
    void changeCanvasOpacity(boolean opaque) {
    }

    /** As AOSP's: the window's title changed, which only accessibility reads. */
    public void onWindowTitleChanged() {
    }

    public void dispatchInvalidateDelayed(View view, long delayMilliseconds) {
        mHandler.sendMessageDelayed(mHandler.obtainMessage(MSG_INVALIDATE, view), delayMilliseconds);
    }

    public void dispatchInvalidateRectDelayed(AttachInfo.InvalidateInfo info, long delayMilliseconds) {
        mHandler.sendMessageDelayed(mHandler.obtainMessage(MSG_INVALIDATE_RECT, info), delayMilliseconds);
    }

    public void dispatchInvalidateOnAnimation(View view) {
        mChoreographer.postCallback(Choreographer.CALLBACK_ANIMATION, view::invalidate, null);
    }

    public void dispatchInvalidateRectOnAnimation(AttachInfo.InvalidateInfo info) {
        mChoreographer.postCallback(Choreographer.CALLBACK_ANIMATION, () -> {
            info.target.invalidate(info.left, info.top, info.right, info.bottom);
            info.recycle();
        }, null);
    }

    public void cancelInvalidate(View view) {
        mHandler.removeMessages(MSG_INVALIDATE, view);
    }

    /** No input reaches a window before ADR-0041 a)'s input. */
    public void dispatchInputEvent(InputEvent event) {
        Log.w(TAG, "No input on Minato's windows yet (ADR-0041 a)): " + event);
    }

    public boolean dispatchUnhandledKeyEvent(KeyEvent event) {
        return false;
    }

    public void dispatchKeyFromAutofill(KeyEvent event) {
    }

    public void touchModeChanged(boolean inTouchMode) {
        mAttachInfo.mInTouchMode = inTouchMode;
    }

    /** As AOSP's for a touch screen: the window stays in touch mode, so nothing changes. */
    boolean ensureTouchMode(boolean inTouchMode) {
        return false;
    }

    public boolean probablyHasInput() {
        return false;
    }

    boolean isInLayout() {
        return mInLayout;
    }

    /** As AOSP's: a layout requested during layout runs at the next traversal. */
    boolean requestLayoutDuringLayout(View view) {
        if (view.mParent == null || view.mAttachInfo == null) {
            return true;
        }
        mLayoutRequested = true;
        scheduleTraversals();
        return false;
    }

    public static boolean isViewDescendantOf(View child, View parent) {
        if (child == parent) {
            return true;
        }
        final ViewParent theParent = child.getParent();
        return (theParent instanceof ViewGroup) && isViewDescendantOf((View) theParent, parent);
    }

    /** The window's frame in the display, which the window covers whole. */
    void getDisplayFrame(Rect outFrame) {
        outFrame.set(0, 0, mWidth, mHeight);
    }

    void getWindowVisibleDisplayFrame(Rect outFrame) {
        outFrame.set(0, 0, mWidth, mHeight);
    }

    void applyViewBoundsSandboxingIfNeeded(final Rect inOutRect) {
    }

    public void applyViewLocationSandboxingIfNeeded(int[] outLocation) {
    }

    void transformMatrixToGlobal(Matrix m) {
    }

    void transformMatrixToLocal(Matrix m) {
    }

    // The members that come with ADR-0041 a)'s rest: drawing (M4 step 10), input, IME and accessibility.

    public InsetsController getInsetsController() {
        return null;
    }

    /** The window's IME focus, which reaches no input method yet (ImeFocusController). */
    public ImeFocusController getImeFocusController() {
        return mImeFocusController;
    }

    /** The window's stylus handwriting, which starts none (HandwritingInitiator). */
    public HandwritingInitiator getHandwritingInitiator() {
        return mHandwritingInitiator;
    }

    public void reportDrawFinish() {
    }

    public void registerAnimatingRenderNode(RenderNode animator) {
    }

    public void registerVectorDrawableAnimator(NativeVectorDrawableAnimator animator) {
    }

    public void requestTransitionStart(LayoutTransition transition) {
    }

    public void notifyRendererOfExpensiveFrame() {
    }

    public void requestInvalidateRootRenderNode() {
    }

    void addSensitiveContentAppProtection() {
    }

    void removeSensitiveContentAppProtection() {
    }

    public void dispatchCompatFakeFocus() {
    }

    void updateSystemGestureExclusionRectsForView(View view) {
    }

    public void updateDecorViewGestureInterception(boolean intercepted) {
    }

    void updateKeepClearRectsForView(View view) {
    }

    public View getAccessibilityFocusedHost() {
        return null;
    }

    void setAccessibilityFocus(View view, AccessibilityNodeInfo node) {
    }

    boolean hasPointerCapture() {
        return false;
    }

    void requestPointerCapture(boolean enabled) {
    }

    public void refreshPointerIcon() {
    }

    void setLocalDragState(Object obj) {
    }

    public void getLastTouchPoint(Point outLocation) {
        outLocation.set(0, 0);
    }

    public int getLastTouchSource() {
        return InputDevice.SOURCE_TOUCHSCREEN;
    }

    public int getLastTouchDeviceId() {
        return 0;
    }

    public int getLastTouchPointerId() {
        return 0;
    }

    public void setDragFocus(View newDragTarget, DragEvent event) {
    }

    void setDragStartedViewForAccessibility(View view) {
    }

    public void addScrollCaptureCallback(ScrollCaptureCallback callback) {
    }

    public void removeScrollCaptureCallback(ScrollCaptureCallback callback) {
    }

    public void dispatchScrollCaptureRequest(IScrollCaptureResponseListener listener) {
    }

    public void votePreferredFrameRateCategory(int frameRateCategory, int reason, View view) {
    }

    public void votePreferredFrameRate(float frameRate, int frameRateCompatibility) {
    }

    public void addThreadedRendererView(View view) {
    }

    public void removeThreadedRendererView(View view) {
    }

    int intermittentUpdateState() {
        return INTERMITTENT_STATE_NOT_INTERMITTENT;
    }

    public boolean shouldCheckFrameRateCategory() {
        return false;
    }

    public boolean shouldCheckFrameRate(boolean isDirect) {
        return false;
    }

    void recordViewPercentage(float percentage) {
    }

    public void dump(String prefix, PrintWriter writer) {
        writer.println(prefix + "ViewRootImpl " + mWindowAttributes.getTitle() + ": " + mWidth + " x " + mHeight
                + (mAttachInfo.mHasWindowFocus ? ", focused" : ""));
    }
}
