/*
 * Our ViewRootImpl, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3): the root of one window, the parent
 * of its decor view. AOSP's (13,254 lines) asks system_server's window manager for its frame, surface and input; ours
 * lays out locally:
 *   - setView attaches the window as AOSP's does after addToDisplayAsUser: the view gets this as its parent, the
 *     window is in touch mode, as a touch screen's starts, and a traversal is scheduled;
 *   - a traversal runs as AOSP's on Choreographer's CALLBACK_TRAVERSAL behind a sync barrier. The first attaches the
 *     view tree, dispatches the window's insets and measures the tree at the display's size, which the window
 *     covers whole (ADR-0038), or at the host's window's size once the session gave it; each traversal lays out
 *     what requested it, dispatches the global layout and the pre-draw, and then draws as AOSP's software path does
 *     (ADR-0043 c3): the dirty rectangle the invalidations collected, or the whole window after a new surface or
 *     size, on the canvas of the surface the session's host window gives (WindowSurface), whose format follows the
 *     window's attributes as BLASTBufferQueue's. The host's session gives the window its focus (ADR-0043 step c1);
 *     without a session the window has no surface and draws nothing, and gains focus after the first traversal, as a
 *     new activity's window does once system_server shows it;
 *   - the insets are none until ADR-0041 decision 8, so a window that fits the system windows keeps its whole size,
 *     as an edge-to-edge window does on Android 15. The window's InsetsController, ours, keeps what the app asks of it,
 *     and setView replays into it what the app asked of the decor view's pending one (ADR-0042 b's start stops);
 *   - the back dispatcher is AOSP's, local: without a window session it keeps the callbacks and tells no one
 *     (ADR-0041 c) wires the back gesture);
 *   - touches come through the session's input channel, which ActivityThread hands over (ADR-0045 step 7b2): a
 *     WindowInputEventReceiver delivers them as AOSP's input stages deliver a touch, MOVEs at the frame's
 *     CALLBACK_INPUT;
 *   - the members that View, ViewGroup, Window and Activity reach answer as AOSP's does for a window without keys,
 *     drawing, accessibility or IME focus, which come with ADR-0041 a)'s rest.
 */
package android.view;

import android.animation.LayoutTransition;
import android.content.Context;
import android.app.ActivityThread;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.Region;
import android.graphics.RenderNode;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.util.TimeUtils;
import android.view.View.AttachInfo;
import android.view.Window.OnContentApplyWindowInsetsListener;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.window.ActivityWindowInfo;
import android.window.WindowOnBackInvokedDispatcher;
import com.android.internal.policy.DecorView;
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
    private static final int MSG_CHECK_FOCUS = 5;

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
    /**
     * The input frame's statistics, which View reads only while it handles a pointer event, under the flag
     * adpfMeasureDuringInputEventBoost, which the framework's flags leave off (ADR-0045 step 7b2).
     */
    protected final ViewFrameInfo mViewFrameInfo = null;
    /** The window's surface, on the session's host window from NATIVE_WINDOW_CREATED to its DESTROYED (c3). */
    public final Surface mSurface = new Surface();
    int mCurScrollY;
    /** As AOSP's: what the next draw redraws, in the window's pixels; invalidateRectOnScreen unions into it. */
    final Rect mDirty = new Rect();
    /** As AOSP's: a traversal runs, which draws what is invalidated meanwhile without another one. */
    boolean mWillDrawSoon;
    /** As AOSP's: the next draw takes the whole window, after a new surface or size and at the host's REDRAW_NEEDED. */
    boolean mFullRedrawNeeded;
    /** As AOSP's: the attributes changed, so the next traversal decides the window's format again. */
    boolean mWindowAttributesChanged;
    /** The software canvas's densities, as AOSP's: the display's. */
    private final int mDensity;
    private final int mNoncompatDensity;
    /** The window's format as the last traversal decided it (a PixelFormat), 0 before the first. */
    private int mWindowFormat;
    /** The format the session's host was told last (WINDOW_FORMAT), none before the first. */
    private int mReportedFormat = Integer.MIN_VALUE;

    final AttachInfo mAttachInfo;
    final ViewRootHandler mHandler = new ViewRootHandler();
    private final Context mContext;
    private final Display mDisplay;
    private final W mWindow = new W();
    private final WindowOnBackInvokedDispatcher mOnBackInvokedDispatcher;
    private final Rect mTempRect = new Rect();
    private final ImeFocusController mImeFocusController;
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
    // The window's touches (ADR-0045 step 7b2): the session's input channel and its receiver, whose MOVEs wait for
    // the frame's CALLBACK_INPUT unless a view asked for unbuffered dispatch, as AOSP's.
    private InputChannel mInputChannel;
    private WindowInputEventReceiver mInputEventReceiver;
    private boolean mConsumeBatchedInputScheduled;
    private boolean mConsumeBatchedInputImmediatelyScheduled;
    private boolean mUnbufferedInputDispatch;
    private int mUnbufferedInputSource = InputDevice.SOURCE_CLASS_NONE;
    private final ConsumeBatchedInputRunnable mConsumedBatchedInputRunnable = new ConsumeBatchedInputRunnable();
    private final ConsumeBatchedInputImmediatelyRunnable mConsumeBatchedInputImmediatelyRunnable =
            new ConsumeBatchedInputImmediatelyRunnable();
    private boolean mApplyInsetsRequested;
    private boolean mUpcomingWindowFocus;
    private boolean mWindowFocusChanged;
    /** The host's focus, held until the window drew with its SurfaceViews' surfaces (releaseHeldFocus). */
    private boolean mFocusHeld;
    /** Whether the window drew since its host window came. */
    private boolean mDrawnSinceHostWindow;
    private int mWidth = -1;
    private int mHeight = -1;
    /** Whether the host's session gives the windows, their sizes and their focus (ADR-0043 step c1). */
    private static boolean sHostWindows;
    /** The host's window, from its session's events: its ID, 0 without one, and its size, 0 before the first. */
    private long mHostWindow;
    /** Our SurfaceViews, which hear of the window's surface and stop (ADR-0043 c4). */
    private final ArrayList<WindowSurfaceCallback> mWindowSurfaceCallbacks = new ArrayList<>();
    private int mHostWidth;
    private int mHostHeight;
    private WindowInsets mLastWindowInsets;
    /** The window's insets controller, a recorder until decision 8's insets (ours, under AOSP's name). */
    private final InsetsController mInsetsController = new InsetsController();

    public ViewRootImpl(Context context, Display display) {
        mContext = context;
        mDisplay = display;
        mImeFocusController = new ImeFocusController(this, context);
        mThread = Thread.currentThread();
        mAttachInfo = new AttachInfo(null, mWindow, display, this, mHandler, this, context);
        mDensity = context.getResources().getDisplayMetrics().densityDpi;
        mNoncompatDensity = context.getResources().getDisplayMetrics().noncompatDensityDpi;
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
                case MSG_CHECK_FOCUS:
                    mImeFocusController.onScheduledCheckFocus();
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
            mWindowAttributesChanged = true;
            requestLayout();
            // As AOSP's: what the app asked of the decor view's pending controller goes to the window's.
            if (view instanceof DecorView) {
                ((DecorView) view).providePendingInsetsController().replayAndAttach(mInsetsController);
            }
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
            mWindowAttributesChanged = true;
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
        mWillDrawSoon = true;
        final boolean first = mFirst;
        final int viewVisibility = host.getVisibility();
        Point size = new Point();
        mDisplay.getRealSize(size);
        if (mHostWidth > 0 && mHostHeight > 0) {
            size.set(mHostWidth, mHostHeight);
        }
        if (first) {
            mFullRedrawNeeded = true;
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
        // As AOSP's: changed attributes decide the window's format, translucent once the tree asks for a transparent
        // region, which relayoutWindow then gives the window's surface.
        if (mWindowAttributesChanged) {
            mWindowAttributesChanged = false;
            final WindowManager.LayoutParams params = mWindowAttributes;
            if ((host.mPrivateFlags & View.PFLAG_REQUEST_TRANSPARENT_REGIONS) != 0
                    && !PixelFormat.formatHasAlpha(params.format)) {
                params.format = PixelFormat.TRANSLUCENT;
            }
            setWindowFormat(params.format);
        }
        boolean triggerGlobalLayoutListener = false;
        if (mLayoutRequested) {
            mLayoutRequested = false;
            final WindowManager.LayoutParams lp = mWindowAttributes;
            host.measure(getRootMeasureSpec(size.x, lp.width), getRootMeasureSpec(size.y, lp.height));
            if (host.getMeasuredWidth() != mWidth || host.getMeasuredHeight() != mHeight) {
                // As AOSP's for a new frame size: the whole window again.
                mFullRedrawNeeded = true;
            }
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
        mImeFocusController.onTraversal(mAttachInfo.mHasWindowFocus && viewVisibility == View.VISIBLE,
                mWindowAttributes);
        if (first) {
            // As AOSP's first traversal in touch mode: a focused group that wants its children first gives way.
            View focused = host.findFocus();
            if (focused instanceof ViewGroup group
                    && group.getDescendantFocusability() == ViewGroup.FOCUS_AFTER_DESCENDANTS) {
                focused.restoreDefaultFocus();
            }
        }
        mFirst = false;
        mWillDrawSoon = false;
        // As AOSP's: a window that is not visible draws nothing, and a pre-draw listener that cancels the draw gets
        // another traversal at the next frame.
        boolean cancelDraw = mAttachInfo.mTreeObserver.dispatchOnPreDraw();
        if (viewVisibility == View.VISIBLE) {
            if (cancelDraw) {
                scheduleTraversals();
            } else if (performDraw()) {
                mDrawnSinceHostWindow = true;
                releaseHeldFocus();
            }
        }
        if (first && !sHostWindows) {
            windowFocusChanged(true);
        }
    }

    /** @hide ActivityThread's, once it attached the host's session: the host gives the windows and their focus. */
    public static void setHostWindows(boolean hostWindows) {
        sHostWindows = hostWindows;
    }

    /**
     * @hide What our SurfaceView hears of its window (ADR-0043 c4), as AOSP's SurfaceChangedCallback tells it and as
     * AOSP's stop reaches it: the window's own surface made or gone, and the window stopped or started.
     */
    public interface WindowSurfaceCallback {
        void windowSurfaceCreated();

        void windowSurfaceDestroyed();

        void windowStopped(boolean stopped);

        /** Whether its surface was asked for and has not come yet. */
        boolean surfacePending();
    }

    /** @hide SurfaceView's, at its attach. */
    public void addWindowSurfaceCallback(WindowSurfaceCallback callback) {
        mWindowSurfaceCallbacks.add(callback);
    }

    /** @hide SurfaceView's, at its detach. */
    public void removeWindowSurfaceCallback(WindowSurfaceCallback callback) {
        mWindowSurfaceCallbacks.remove(callback);
    }

    /** @hide Whether the window has the host's window, and so its own surface. */
    boolean hasHostWindow() {
        return mHostWindow != 0;
    }

    /** @hide Whether the window is stopped (setWindowStopped). */
    boolean isWindowStopped() {
        return mStopped;
    }

    /**
     * @hide ActivityThread's, at the session's NATIVE_WINDOW_CREATED and RESIZED: the host's window and its size in
     * pixels, at which the next traversal lays the tree out; at NATIVE_WINDOW_DESTROYED, ID 0, and the size stays. A
     * new window gives the window's surface, as relayoutWindow's new surface does, in the window's format, and the
     * next draw takes the whole window (ADR-0043 c3); ID 0 releases it.
     */
    public void setHostWindow(long id, int width, int height) {
        if (id != mHostWindow) {
            mHostWindow = id;
            mDrawnSinceHostWindow = false;
            if (id != 0) {
                try {
                    mSurface.transferFrom(WindowSurface.create(id, width, height));
                    setBuffersFormat();
                } catch (Surface.OutOfResourcesException e) {
                    Log.e(TAG, "No surface on the host's window " + id, e);
                }
                mFullRedrawNeeded = true;
                scheduleTraversals();
                for (WindowSurfaceCallback callback : mWindowSurfaceCallbacks.toArray(new WindowSurfaceCallback[0])) {
                    callback.windowSurfaceCreated();
                }
            } else {
                // As AOSP's notifySurfaceDestroyed: the SurfaceViews' surfaces go before the window's own.
                for (WindowSurfaceCallback callback : mWindowSurfaceCallbacks.toArray(new WindowSurfaceCallback[0])) {
                    callback.windowSurfaceDestroyed();
                }
                mSurface.release();
            }
        }
        if (id != 0 && (width != mHostWidth || height != mHostHeight)) {
            mHostWidth = width;
            mHostHeight = height;
            requestLayout();
        }
    }

    /**
     * @hide ActivityThread's, at the session's NATIVE_WINDOW_REDRAW_NEEDED: the whole window again at the next
     * traversal, as a draw the window manager syncs is a full one.
     */
    public void redrawNeeded() {
        mFullRedrawNeeded = true;
        scheduleTraversals();
    }

    /**
     * The window's format, as relayoutWindow hands it to the window manager (ADR-0043 c3): its surface's buffers take
     * it, and the session's host learns it, as ANativeActivity_setWindowFormat tells it, whose opacity the runtime
     * reports.
     */
    private void setWindowFormat(int format) {
        mWindowFormat = format;
        setBuffersFormat();
        if (sHostWindows && format != mReportedFormat) {
            mReportedFormat = format;
            ActivityThread.requestWindowFormat(format);
        }
    }

    /**
     * The surface's buffers in the window's format, as BLASTBufferQueue's convertBufferFormat gives it: OPAQUE as
     * RGBX_8888, TRANSLUCENT and TRANSPARENT as RGBA_8888, any other as it is. The host's buffers take RGBA_8888,
     * RGBX_8888 and RGB_565; another format fails at the lock.
     */
    private void setBuffersFormat() {
        if (!mSurface.isValid() || mWindowFormat == 0) {
            return;
        }
        int format = mWindowFormat;
        if (format == PixelFormat.TRANSPARENT || format == PixelFormat.TRANSLUCENT) {
            format = PixelFormat.RGBA_8888;
        } else if (format == PixelFormat.OPAQUE) {
            format = PixelFormat.RGBX_8888;
        }
        WindowSurface.setBuffersGeometry(mSurface, 0, 0, format);
    }

    /** As AOSP's, without a hardware renderer, a sync or the screen's state: the draw, once per traversal. */
    private boolean performDraw() {
        if (mAttachInfo.mDisplayState == Display.STATE_OFF || mView == null) {
            return false;
        }
        final boolean fullRedrawNeeded = mFullRedrawNeeded;
        mFullRedrawNeeded = false;
        return draw(fullRedrawNeeded);
    }

    /**
     * As AOSP's for software: the dirty rectangle, or the whole window, drawn on the surface's canvas, after the tree
     * observer's onDraw, with the drawing time from Choreographer's frame. Without a scroller or surface insets the
     * canvas is not offset, and without accessibility no focus is drawn.
     */
    private boolean draw(boolean fullRedrawNeeded) {
        final Surface surface = mSurface;
        if (!surface.isValid()) {
            return false;
        }
        if (mAttachInfo.mViewScrollChanged) {
            mAttachInfo.mViewScrollChanged = false;
            mAttachInfo.mTreeObserver.dispatchOnScrollChanged();
        }
        final float appScale = mAttachInfo.mApplicationScale;
        final boolean scalingRequired = mAttachInfo.mScalingRequired;
        final Rect dirty = mDirty;
        if (fullRedrawNeeded) {
            dirty.set(0, 0, (int) (mWidth * appScale + 0.5f), (int) (mHeight * appScale + 0.5f));
        }
        mAttachInfo.mTreeObserver.dispatchOnDraw();
        mAttachInfo.mDrawingTime = mChoreographer.getFrameTimeNanos() / TimeUtils.NANOS_PER_MS;
        if (!dirty.isEmpty() || mIsAnimating) {
            return drawSoftware(surface, 0, mCurScrollY, scalingRequired, dirty);
        }
        return false;
    }

    /**
     * As AOSP's: the canvas locked on the dirty rectangle at the display's density, cleared when its buffers have
     * alpha, the tree drawn and posted. A surface that fails asks for a layout, as AOSP's asks the window manager for a
     * new one; ours logs it, as no window manager has memory to free.
     */
    @SuppressWarnings("finally") // the post's failure returns from the finally block, as AOSP's does.
    private boolean drawSoftware(Surface surface, int xoff, int yoff, boolean scalingRequired, Rect dirty) {
        final Canvas canvas;
        try {
            canvas = mSurface.lockCanvas(dirty);
            canvas.setDensity(mDensity);
        } catch (Surface.OutOfResourcesException e) {
            Log.e(TAG, "OutOfResourcesException locking the surface", e);
            mLayoutRequested = true;
            return false;
        } catch (IllegalArgumentException e) {
            Log.e(TAG, "Could not lock surface", e);
            mLayoutRequested = true;
            return false;
        }
        try {
            // As AOSP's: a canvas with alpha starts clear within the dirty rectangle, so the tree composes over
            // transparency; an offset canvas, so that nothing old shows in its blank part.
            if (!canvas.isOpaque() || yoff != 0 || xoff != 0) {
                canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            }
            dirty.setEmpty();
            mIsAnimating = false;
            mView.mPrivateFlags |= View.PFLAG_DRAWN;
            canvas.translate(-xoff, -yoff);
            canvas.setScreenDensity(scalingRequired ? mNoncompatDensity : 0);
            mView.draw(canvas);
        } finally {
            try {
                surface.unlockCanvasAndPost(canvas);
            } catch (IllegalArgumentException e) {
                Log.e(TAG, "Could not unlock surface", e);
                mLayoutRequested = true;
                return false;
            }
        }
        return true;
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
        // As Android's window manager gives a window focus only once it drew, its SurfaceViews' surfaces with it
        // (ADR-0043 c4): the host's comes at once, so it waits for the window's first draw on its host window and for
        // every surface asked for; a loss while it waits is no change for the views.
        if (hasWindowFocus && !readyForFocus()) {
            mFocusHeld = true;
            return;
        }
        if (!hasWindowFocus && mFocusHeld) {
            mFocusHeld = false;
            if (!mAttachInfo.mHasWindowFocus) {
                return;
            }
        }
        mFocusHeld = false;
        if (mAdded && mView != null) {
            mAttachInfo.mHasWindowFocus = hasWindowFocus;
            // As AOSP's: the input method manager hears of the focus around the views' callbacks.
            mImeFocusController.onPreWindowFocus(hasWindowFocus, mWindowAttributes);
            mAttachInfo.mKeyDispatchState.reset();
            mView.dispatchWindowFocusChanged(hasWindowFocus);
            mAttachInfo.mTreeObserver.dispatchOnWindowFocusChange(hasWindowFocus);
            mImeFocusController.onPostWindowFocus(mView.findFocus(), hasWindowFocus, mWindowAttributes);
            if (hasWindowFocus) {
                mWindowAttributes.softInputMode &= ~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
                ((WindowManager.LayoutParams) mView.getLayoutParams()).softInputMode &=
                        ~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
            }
        }
    }

    /** Whether the host's focus may reach the views: no host window, or one drawn on with every surface there. */
    private boolean readyForFocus() {
        if (mHostWindow == 0) {
            return true;
        }
        if (!mDrawnSinceHostWindow) {
            return false;
        }
        for (WindowSurfaceCallback callback : mWindowSurfaceCallbacks) {
            if (callback.surfacePending()) {
                return false;
            }
        }
        return true;
    }

    /** @hide The window's draw and SurfaceView's surface (ADR-0043 c4): a held focus goes once nothing waits. */
    void releaseHeldFocus() {
        if (mFocusHeld && readyForFocus()) {
            mFocusHeld = false;
            windowFocusChanged(true);
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
                mImeFocusController.onWindowDismissed();
                mView.assignParent(null);
                mView = null;
                mAttachInfo.mRootView = null;
            }
            mSurface.release();
            mAdded = false;
            unscheduleTraversals();
            disposeInput();
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

    /** As AOSP's: the window turns translucent at the next traversal (the format's part of ADR-0043 c3). */
    @Override
    public void requestTransparentRegion(View child) {
        checkThread();
        if (mView != child) {
            return;
        }
        if ((mView.mPrivateFlags & View.PFLAG_REQUEST_TRANSPARENT_REGIONS) == 0) {
            mView.mPrivateFlags |= View.PFLAG_REQUEST_TRANSPARENT_REGIONS;
            mWindowAttributesChanged = true;
        }
        requestLayout();
    }

    @Override
    public void invalidateChild(View child, Rect dirty) {
        invalidateChildInParent(null, dirty);
    }

    /** As AOSP's: the whole window at the next traversal. */
    void invalidate() {
        mDirty.set(0, 0, mWidth, mHeight);
        if (!mWillDrawSoon) {
            scheduleTraversals();
        }
    }

    /** As AOSP's, without a compatibility translator: the child's rectangle on the window, below its scroll. */
    @Override
    public ViewParent invalidateChildInParent(int[] location, Rect dirty) {
        checkThread();
        if (dirty == null) {
            invalidate();
            return null;
        } else if (dirty.isEmpty() && !mIsAnimating) {
            return null;
        }
        if (mCurScrollY != 0) {
            mTempRect.set(dirty);
            dirty = mTempRect;
            dirty.offset(0, -mCurScrollY);
        }
        invalidateRectOnScreen(dirty);
        return null;
    }

    /** As AOSP's: the rectangle joins the dirty one, cut to the window, and a traversal comes unless one runs. */
    private void invalidateRectOnScreen(Rect dirty) {
        final Rect localDirty = mDirty;
        localDirty.union(dirty.left, dirty.top, dirty.right, dirty.bottom);
        final float appScale = mAttachInfo.mApplicationScale;
        final boolean intersected = localDirty.intersect(0, 0,
                (int) (mWidth * appScale + 0.5f), (int) (mHeight * appScale + 0.5f));
        if (!intersected) {
            localDirty.setEmpty();
        }
        if (!mWillDrawSoon && (intersected || mIsAnimating)) {
            scheduleTraversals();
        }
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
            for (WindowSurfaceCallback callback : mWindowSurfaceCallbacks.toArray(new WindowSurfaceCallback[0])) {
                callback.windowStopped(stopped);
            }
        }
    }

    /** The window's opacity for the hardware path's canvas; the software path takes its buffers' format. */
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

    /**
     * The window's input channel (ADR-0045 step 7b2), which ActivityThread makes of the session's at
     * INPUT_QUEUE_CREATED, as AOSP's setView gets one from the window manager: a WindowInputEventReceiver on this
     * thread's looper delivers its touches. Null, at INPUT_QUEUE_DESTROYED, takes both away.
     */
    public void setInputChannel(InputChannel inputChannel) {
        disposeInput();
        mInputChannel = inputChannel;
        if (inputChannel != null) {
            mInputEventReceiver = new WindowInputEventReceiver(inputChannel, Looper.myLooper());
        }
    }

    /** As AOSP's dispatchDetachedFromWindow: the receiver, then the channel. */
    private void disposeInput() {
        if (mInputEventReceiver != null) {
            mInputEventReceiver.dispose();
            mInputEventReceiver = null;
        }
        if (mInputChannel != null) {
            mInputChannel.dispose();
            mInputChannel = null;
        }
    }

    /**
     * AOSP's WindowInputEventReceiver for touches, without InputEventCompatProcessor, which only changes a stylus's
     * buttons for targets below 23, and without the focus, capture, drag and touch mode events, which the session's
     * channel does not carry.
     */
    final class WindowInputEventReceiver extends InputEventReceiver {
        WindowInputEventReceiver(InputChannel inputChannel, Looper looper) {
            super(inputChannel, looper);
        }

        @Override
        public void onInputEvent(InputEvent event) {
            deliverInputEvent(event, this);
        }

        @Override
        public void onBatchedInputEventPending(int source) {
            final boolean unbuffered = mUnbufferedInputDispatch
                    || (source & mUnbufferedInputSource) != InputDevice.SOURCE_CLASS_NONE;
            if (unbuffered) {
                if (mConsumeBatchedInputScheduled) {
                    unscheduleConsumeBatchedInput();
                }
                // Consume event immediately if unbuffered input dispatch has been requested.
                consumeBatchedInputEvents(-1);
                return;
            }
            scheduleConsumeBatchedInput();
        }

        @Override
        public void dispose() {
            unscheduleConsumeBatchedInput();
            super.dispose();
        }
    }

    /** As AOSP's: a view asked for unbuffered dispatch of a source's events (View.requestUnbufferedDispatch). */
    @Override
    public void onDescendantUnbufferedRequested() {
        mUnbufferedInputSource = mView.mUnbufferedInputSource;
    }

    void scheduleConsumeBatchedInput() {
        // If anything is currently scheduled to consume batched input then there's no point in scheduling it again.
        if (!mConsumeBatchedInputScheduled && !mConsumeBatchedInputImmediatelyScheduled) {
            mConsumeBatchedInputScheduled = true;
            mChoreographer.postCallback(Choreographer.CALLBACK_INPUT, mConsumedBatchedInputRunnable, null);
        }
    }

    void unscheduleConsumeBatchedInput() {
        if (mConsumeBatchedInputScheduled) {
            mConsumeBatchedInputScheduled = false;
            mChoreographer.removeCallbacks(Choreographer.CALLBACK_INPUT, mConsumedBatchedInputRunnable, null);
        }
    }

    void scheduleConsumeBatchedInputImmediately() {
        if (!mConsumeBatchedInputImmediatelyScheduled) {
            unscheduleConsumeBatchedInput();
            mConsumeBatchedInputImmediatelyScheduled = true;
            mHandler.post(mConsumeBatchedInputImmediatelyRunnable);
        }
    }

    boolean doConsumeBatchedInput(long frameTimeNanos) {
        return mInputEventReceiver != null && mInputEventReceiver.consumeBatchedInputEvents(frameTimeNanos);
    }

    final class ConsumeBatchedInputRunnable implements Runnable {
        @Override
        public void run() {
            mConsumeBatchedInputScheduled = false;
            if (doConsumeBatchedInput(mChoreographer.getFrameTimeNanos())) {
                // If we consumed a batch here, we want to go ahead and schedule the consumption of batched input
                // events on the next frame, as AOSP's does.
                scheduleConsumeBatchedInput();
            }
        }
    }

    final class ConsumeBatchedInputImmediatelyRunnable implements Runnable {
        @Override
        public void run() {
            mConsumeBatchedInputImmediatelyScheduled = false;
            doConsumeBatchedInput(-1);
        }
    }

    /**
     * AOSP's deliverInputEvent and its input stages for a touch, in one: a focus change still posted reaches the views
     * first, as deliverInputEvent's handleWindowFocusChanged before the stages; an event the window cannot take now is
     * dropped, or cancelled when it ends a gesture (InputStage.shouldDropInputEvent); then EarlyPostImeInputStage's and
     * ViewPostImeInputStage's processPointerEvent; the end of unbuffered dispatch at a touch's end, handled or dropped,
     * as ViewPostImeInputStage.onDeliverToNext; and the answer. Keys come with step 7c.
     */
    private void deliverInputEvent(InputEvent event, InputEventReceiver receiver) {
        handleWindowFocusChanged();
        boolean handled = false;
        if (event instanceof MotionEvent && event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            final MotionEvent motion = (MotionEvent) event;
            if (!shouldDropInputEvent(motion)) {
                handled = processPointerEvent(motion);
            }
            if (mUnbufferedInputDispatch && motion.isTouchEvent() && isTerminalInputEvent(motion)) {
                mUnbufferedInputDispatch = false;
                scheduleConsumeBatchedInput();
            }
        }
        receiver.finishInputEvent(event, handled);
    }

    /** As AOSP's for a motion event: an UP, a CANCEL or a HOVER_EXIT ends what came before it. */
    private static boolean isTerminalInputEvent(MotionEvent event) {
        final int action = event.getAction();
        return action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
                || action == MotionEvent.ACTION_HOVER_EXIT;
    }

    private boolean shouldDropInputEvent(MotionEvent event) {
        if (mView == null || !mAdded) {
            Log.w(TAG, "Dropping event due to root view being removed: " + event);
            return true;
        }
        if (!mStopped) {
            return false;
        }
        if (isTerminalInputEvent(event)) {
            // Don't drop terminal input events, however mark them as canceled.
            event.cancel();
            Log.w(TAG, "Cancelling event (window is stopped):" + event);
            return false;
        }
        Log.w(TAG, "Dropping event (window is stopped): " + event);
        return true;
    }

    private boolean processPointerEvent(MotionEvent event) {
        // EarlyPostImeInputStage: touch mode at a DOWN or a scroll from any device, no tooltip at a DOWN, the scroll's
        // offset. Left out: the compatibility translator (mTranslator), for screen-compat apps only; the autofill UI's
        // hide at a DOWN (requestHideFillUi); the last touch for a drag's start and the IME's last UP, which come with
        // drags and the IME.
        final int action = event.getAction();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_SCROLL) {
            ensureTouchMode(true);
        }
        if (action == MotionEvent.ACTION_DOWN && mAttachInfo.mTooltipHost != null) {
            mAttachInfo.mTooltipHost.hideTooltip();
        }
        if (mCurScrollY != 0) {
            event.offsetLocation(0, mCurScrollY);
        }
        // ViewPostImeInputStage. Left out: the handwriting initiator, which starts only with a stylus; the pointer
        // icon's and the tooltip's updates (maybeUpdatePointerIcon, maybeUpdateTooltip), for mice and hovers only; the
        // touch boost of variable refresh rates.
        mAttachInfo.mUnbufferedDispatchRequested = false;
        mAttachInfo.mHandlingPointerEvent = true;
        final boolean handled = mView.dispatchPointerEvent(event);
        mAttachInfo.mHandlingPointerEvent = false;
        if (mAttachInfo.mUnbufferedDispatchRequested && !mUnbufferedInputDispatch) {
            mUnbufferedInputDispatch = true;
            if (mConsumeBatchedInputScheduled) {
                scheduleConsumeBatchedInputImmediately();
            }
        }
        return handled;
    }

    /**
     * An event handed in directly, not through the window's input channel: an input method's, accessibility's or an
     * injected one, which come with ADR-0045 step 7c. Touches come through the channel (ADR-0045 step 7b2).
     */
    public void dispatchInputEvent(InputEvent event) {
        Log.w(TAG, "No events handed in directly on Minato's windows yet (ADR-0045 step 7c): " + event);
    }

    public boolean dispatchUnhandledKeyEvent(KeyEvent event) {
        return false;
    }

    public void dispatchKeyFromAutofill(KeyEvent event) {
    }

    /** A key an input method sends, which reaches the window as input does: not yet (ADR-0041 a)). */
    public void dispatchKeyFromIme(KeyEvent event) {
        dispatchInputEvent(event);
    }

    public void touchModeChanged(boolean inTouchMode) {
        mAttachInfo.mInTouchMode = inTouchMode;
    }

    /** As AOSP's for a touch screen: the window stays in touch mode, so nothing changes. */
    boolean ensureTouchMode(boolean inTouchMode) {
        return false;
    }

    /** As AOSP's: whether the receiver has a touch waiting (ADR-0045 step 7b2). */
    public boolean probablyHasInput() {
        return mInputEventReceiver != null && mInputEventReceiver.probablyHasInput();
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

    // The members that come with ADR-0041 a)'s rest: the hardware renderer, keys, IME and accessibility.

    public InsetsController getInsetsController() {
        return mInsetsController;
    }

    /**
     * As AOSP's: what hears of the process's configuration changes, a text iterator's locale among them
     * (AccessibilityIterators, ADR-0043 step d)). Our windows take no configuration change yet, so none is told.
     */
    public interface ConfigChangedCallback {
        void onConfigurationChanged(Configuration globalConfig);
    }

    private static final ArrayList<ConfigChangedCallback> sConfigCallbacks = new ArrayList<>();

    public static void addConfigCallback(ConfigChangedCallback callback) {
        synchronized (sConfigCallbacks) {
            sConfigCallbacks.add(callback);
        }
    }

    public static void removeConfigCallback(ConfigChangedCallback callback) {
        synchronized (sConfigCallbacks) {
            sConfigCallbacks.remove(callback);
        }
    }

    /** As AOSP's: the input method manager checks the focused view at the next message. */
    public void dispatchCheckFocus() {
        if (!mHandler.hasMessages(MSG_CHECK_FOCUS)) {
            mHandler.sendEmptyMessage(MSG_CHECK_FOCUS);
        }
    }

    /** The window's IME focus, which our InputMethodManager serves without an input method (ImeFocusController). */
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
