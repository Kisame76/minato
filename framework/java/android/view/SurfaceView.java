/*
 * Our SurfaceView, under AOSP's name and with its API (ADR-0043 step c4; ADR-0041 decision 3): AOSP's stands on
 * SurfaceControl transactions and BLASTBufferQueue, which Minato has none of. Ours asks the session for a host layer of
 * its own at its frame in the window, below the window's content or above it (com.android.minato.view.SurfaceState),
 * makes its Surface on the window the host answers with, as our ViewRootImpl does for its own, and gives
 * SurfaceHolder's callbacks in AOSP's order: created, changed and redraw needed for a new surface, changed and redraw
 * needed for a new size, destroyed before the surface goes (SurfaceView.java at android-15.0.0_r1). It places itself
 * where AOSP's updateSurface runs: at the pre-draw, a scroll, a new frame and a change of its visibility or its
 * window's. Below the window, it punches its hole with Canvas.punchHole and asks for a transparent region, which makes
 * the window translucent (ADR-0043 decision 4). A host without surfaces is logged once, and the view then has none.
 */
package android.view;

import android.app.ActivityThread;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.util.Log;

import com.android.minato.view.SurfaceState;

import java.util.ArrayList;
import java.util.concurrent.locks.ReentrantLock;

public class SurfaceView extends View implements ViewRootImpl.WindowSurfaceCallback {
    private static final String TAG = "SurfaceView";

    /** AOSP's lifecycle strategies (setSurfaceLifecycle, API 34); Minato's surface always follows visibility. */
    public static final int SURFACE_LIFECYCLE_DEFAULT = 0;
    public static final int SURFACE_LIFECYCLE_FOLLOWS_VISIBILITY = 1;
    public static final int SURFACE_LIFECYCLE_FOLLOWS_ATTACHMENT = 2;

    /** Android's sublayers (WindowManagerPolicyConstants): media, media overlay, panel. */
    private static final int MEDIA_SUBLAYER = -2;
    private static final int MEDIA_OVERLAY_SUBLAYER = -1;
    private static final int PANEL_SUBLAYER = 1;

    /** The attached SurfaceViews, to which the session's surface events go (main thread only). */
    private static final ArrayList<SurfaceView> sAttached = new ArrayList<>();
    /** Whether a host without surfaces was logged. */
    private static boolean sUnsupportedLogged;

    final ArrayList<SurfaceHolder.Callback> mCallbacks = new ArrayList<>();
    final ReentrantLock mSurfaceLock = new ReentrantLock();
    final Surface mSurface = new Surface();
    final Rect mSurfaceFrame = new Rect();
    int mRequestedWidth = -1;
    int mRequestedHeight = -1;
    int mRequestedFormat = PixelFormat.RGB_565;
    int mRequestedSubLayer = MEDIA_SUBLAYER;
    boolean mIsCreating;
    boolean mViewVisible;
    boolean mDrawFinished;
    boolean mSecure;
    long mLastLockTime;
    private boolean mGlobalListenersAdded;
    private ViewRootImpl mViewRoot;
    private final int[] mLocation = new int[2];

    /**
     * The surface's session: c4's surface on the session's window, or, in a requested window, a sub-window of it
     * (ADR-0041 gap 4 (b3), decision 7), as WMS gives a SurfaceView's surface the sublayer's window: MEDIA below,
     * MEDIA_OVERLAY above it, PANEL on top, its frame in the session's pixels. The kind is the make's until the remove;
     * a sub-window keeps its make's type, so a new z order takes effect with the next surface.
     */
    private final SurfaceState mState = new SurfaceState(new SurfaceState.Session() {
        /** The requested window the surface is a sub-window of, 0 for one on the session's window. */
        private long mParent;
        private int mType;
        private int mFormat;

        @Override
        public int make(long token, int z, int left, int top, int right, int bottom, int width, int height,
                int format) {
            mParent = mViewRoot != null ? mViewRoot.requestToken() : 0;
            if (mParent == 0) {
                return ActivityThread.makeSurface(token, z, left, top, right, bottom, width, height, format);
            }
            mType = z == MEDIA_SUBLAYER ? WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA
                    : z == MEDIA_OVERLAY_SUBLAYER ? WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA_OVERLAY
                    : WindowManager.LayoutParams.TYPE_APPLICATION_PANEL;
            mFormat = format;
            return ActivityThread.makeWindow(token, mType, mParent, left, top, right, bottom, width, height, format, 0,
                    1f, 0f);
        }

        @Override
        public int place(long token, int z, int left, int top, int right, int bottom, int width, int height) {
            if (mParent == 0) {
                return ActivityThread.placeSurface(token, z, left, top, right, bottom, width, height);
            }
            return ActivityThread.placeWindow(token, mType, mParent, left, top, right, bottom, width, height, mFormat,
                    0, 1f, 0f);
        }

        @Override
        public int remove(long token) {
            return mParent == 0 ? ActivityThread.removeSurface(token) : ActivityThread.removeWindow(token);
        }
    }, new SurfaceState.Callbacks() {
        @Override
        @SuppressWarnings("deprecation") // Surface.transferFrom, as our ViewRootImpl takes its window's surface.
        public void created(long window, int width, int height) {
            try {
                mSurface.transferFrom(WindowSurface.create(window, width, height));
            } catch (Surface.OutOfResourcesException e) {
                Log.e(TAG, "No surface on the host's window " + window, e);
                return;
            }
            mSurfaceFrame.set(0, 0, width, height);
            mIsCreating = true;
            try {
                for (SurfaceHolder.Callback callback : callbacks()) {
                    callback.surfaceCreated(mSurfaceHolder);
                }
            } finally {
                mIsCreating = false;
            }
        }

        @Override
        public void changed(int format, int width, int height) {
            mSurfaceFrame.set(0, 0, width, height);
            mIsCreating = true;
            try {
                for (SurfaceHolder.Callback callback : callbacks()) {
                    callback.surfaceChanged(mSurfaceHolder, format, width, height);
                }
            } finally {
                mIsCreating = false;
            }
        }

        @Override
        public void redrawNeeded() {
            for (SurfaceHolder.Callback callback : callbacks()) {
                if (callback instanceof SurfaceHolder.Callback2) {
                    ((SurfaceHolder.Callback2) callback).surfaceRedrawNeededAsync(mSurfaceHolder, () -> { });
                }
            }
            mDrawFinished = true;
            invalidate();
            if (mViewRoot != null) {
                mViewRoot.releaseHeldFocus();
            }
        }

        @Override
        public void destroyed() {
            mDrawFinished = false;
            for (SurfaceHolder.Callback callback : callbacks()) {
                callback.surfaceDestroyed(mSurfaceHolder);
            }
            mSurfaceLock.lock();
            try {
                mSurface.release();
            } finally {
                mSurfaceLock.unlock();
            }
            mSurfaceFrame.setEmpty();
            invalidate();
        }

        @Override
        public void unsupported(int error) {
            if (!sUnsupportedLogged) {
                sUnsupportedLogged = true;
                Log.w(TAG, "The session gives no surfaces (" + error + "): SurfaceViews stay without one");
            }
        }
    });

    private final ViewTreeObserver.OnScrollChangedListener mScrollChangedListener = this::updateSurface;

    private final ViewTreeObserver.OnPreDrawListener mDrawListener = () -> {
        updateSurface();
        return true;
    };

    public SurfaceView(Context context) {
        this(context, null);
    }

    public SurfaceView(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr) {
        this(context, attrs, defStyleAttr, 0);
    }

    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setWillNotDraw(true);
    }

    /** As AOSP's (@hide): a SurfaceView without the background layer AOSP's makes for a translucent window. */
    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes,
            boolean disableBackgroundLayer) {
        this(context, attrs, defStyleAttr, defStyleRes);
    }

    public SurfaceHolder getHolder() {
        return mSurfaceHolder;
    }

    private SurfaceHolder.Callback[] callbacks() {
        synchronized (mCallbacks) {
            return mCallbacks.toArray(new SurfaceHolder.Callback[0]);
        }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        mViewRoot = getViewRootImpl();
        if (mViewRoot != null) {
            mViewRoot.addWindowSurfaceCallback(this);
            mState.setWindowStopped(mViewRoot.isWindowStopped());
            mState.setWindowSurface(mViewRoot.hasHostWindow());
        }
        sAttached.add(this);
        mState.setAttached(true);
        mViewVisible = getVisibility() == VISIBLE;
        mState.setViewVisible(mViewVisible);
        if (mParent != null) {
            mParent.requestTransparentRegion(this);
        }
        if (!mGlobalListenersAdded) {
            ViewTreeObserver observer = getViewTreeObserver();
            observer.addOnScrollChangedListener(mScrollChangedListener);
            observer.addOnPreDrawListener(mDrawListener);
            mGlobalListenersAdded = true;
        }
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        mState.setWindowVisible(visibility == VISIBLE);
        updateSurface();
    }

    @Override
    public void setVisibility(int visibility) {
        super.setVisibility(visibility);
        boolean visible = visibility == VISIBLE;
        if (visible != mViewVisible) {
            // As AOSP's: the transparent region, which the layout gathers, changes with the visibility.
            mViewVisible = visible;
            requestLayout();
        }
        mState.setViewVisible(visible);
        updateSurface();
    }

    @Override
    protected void onDetachedFromWindow() {
        if (mViewRoot != null) {
            mViewRoot.removeWindowSurfaceCallback(this);
            mViewRoot = null;
        }
        if (mGlobalListenersAdded) {
            ViewTreeObserver observer = getViewTreeObserver();
            observer.removeOnScrollChangedListener(mScrollChangedListener);
            observer.removeOnPreDrawListener(mDrawListener);
            mGlobalListenersAdded = false;
        }
        mState.setAttached(false);
        mState.update();
        sAttached.remove(this);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = mRequestedWidth >= 0
                ? resolveSizeAndState(mRequestedWidth, widthMeasureSpec, 0)
                : getDefaultSize(0, widthMeasureSpec);
        int height = mRequestedHeight >= 0
                ? resolveSizeAndState(mRequestedHeight, heightMeasureSpec, 0)
                : getDefaultSize(0, heightMeasureSpec);
        setMeasuredDimension(width, height);
    }

    /** @hide */
    @Override
    protected boolean setFrame(int left, int top, int right, int bottom) {
        boolean result = super.setFrame(left, top, right, bottom);
        updateSurface();
        return result;
    }

    @Override
    public boolean gatherTransparentRegion(Region region) {
        if (isAboveParent()) {
            return super.gatherTransparentRegion(region);
        }
        boolean opaque = true;
        if ((mPrivateFlags & PFLAG_SKIP_DRAW) == 0) {
            // this view draws, remove it from the transparent region
            opaque = super.gatherTransparentRegion(region);
        } else if (region != null) {
            int w = getWidth();
            int h = getHeight();
            if (w > 0 && h > 0) {
                getLocationInWindow(mLocation);
                // otherwise, punch a hole in the whole hierarchy
                int l = mLocation[0];
                int t = mLocation[1];
                region.op(l, t, l + w, t + h, Region.Op.UNION);
            }
        }
        if (PixelFormat.formatHasAlpha(mRequestedFormat)) {
            opaque = false;
        }
        return opaque;
    }

    @Override
    public void draw(Canvas canvas) {
        if (mDrawFinished && !isAboveParent() && (mPrivateFlags & PFLAG_SKIP_DRAW) == 0) {
            clearSurfaceViewPort(canvas);
        }
        super.draw(canvas);
    }

    @Override
    protected void dispatchDraw(Canvas canvas) {
        if (mDrawFinished && !isAboveParent() && (mPrivateFlags & PFLAG_SKIP_DRAW) == PFLAG_SKIP_DRAW) {
            clearSurfaceViewPort(canvas);
        }
        super.dispatchDraw(canvas);
    }

    /** As AOSP's: the hole below the window's content, through which the surface shows. */
    private void clearSurfaceViewPort(Canvas canvas) {
        canvas.punchHole(0f, 0f, getWidth(), getHeight(), 0f, 0f, getAlpha());
    }

    /** Whether the surface lies above the window's content (setZOrderOnTop). */
    boolean isAboveParent() {
        return mRequestedSubLayer >= 0;
    }

    public void setZOrderMediaOverlay(boolean isMediaOverlay) {
        mRequestedSubLayer = isMediaOverlay ? MEDIA_OVERLAY_SUBLAYER : MEDIA_SUBLAYER;
    }

    /** As AOSP's: from R on, the order changes while the surface lives (setZOrderedOnTop). */
    public void setZOrderOnTop(boolean onTop) {
        setZOrderedOnTop(onTop, getContext().getApplicationInfo().targetSdkVersion > 29);
    }

    public boolean isZOrderedOnTop() {
        return mRequestedSubLayer > 0;
    }

    /** @hide As AOSP's: whether the sublayer changed, applied at once where allowed. */
    public boolean setZOrderedOnTop(boolean onTop, boolean allowDynamicChange) {
        final int subLayer = onTop ? PANEL_SUBLAYER : MEDIA_SUBLAYER;
        if (mRequestedSubLayer == subLayer) {
            return false;
        }
        mRequestedSubLayer = subLayer;
        if (!allowDynamicChange || !mState.isCreated()) {
            return allowDynamicChange;
        }
        updateSurface();
        invalidate();
        return true;
    }

    /** Kept as a flag: Minato's screen has no capture to keep the surface from. */
    public void setSecure(boolean isSecure) {
        mSecure = isSecure;
    }

    /** As setSurfaceLifecycle's default: Minato's surface follows the view's visibility whatever the strategy. */
    public void setSurfaceLifecycle(int lifecycleStrategy) {
    }

    /** Minato's surface has no SurfaceControl (ADR-0043 c4): null, as our ViewRootImpl answers. */
    public SurfaceControl getSurfaceControl() {
        return null;
    }

    /** @hide */
    public boolean isFixedSize() {
        return mRequestedWidth != -1 || mRequestedHeight != -1;
    }

    /** Where AOSP's updateSurface runs: the frame in the window, the size, format and order to the state. */
    protected void updateSurface() {
        int width = getWidth();
        int height = getHeight();
        if (mViewRoot != null && width > 0 && height > 0) {
            getLocationInWindow(mLocation);
            // In a requested window the surface is a sub-window at its place in the session's pixels (decision 7).
            if (mViewRoot.requestToken() != 0) {
                mLocation[0] += mViewRoot.mWinFrame.left;
                mLocation[1] += mViewRoot.mWinFrame.top;
            }
            mState.setFrame(mLocation[0], mLocation[1], mLocation[0] + width, mLocation[1] + height);
        } else {
            mState.setFrame(0, 0, 0, 0);
        }
        mState.setFixedSize(Math.max(mRequestedWidth, 0), Math.max(mRequestedHeight, 0));
        mState.setFormat(mRequestedFormat);
        mState.setZ(mRequestedSubLayer);
        mState.update();
    }

    @Override
    public void windowSurfaceCreated() {
        mState.setWindowSurface(true);
        updateSurface();
    }

    @Override
    public void windowSurfaceDestroyed() {
        mState.setWindowSurface(false);
        updateSurface();
    }

    @Override
    public void windowStopped(boolean stopped) {
        mState.setWindowStopped(stopped);
        updateSurface();
    }

    @Override
    public boolean surfacePending() {
        return mState.token() != 0 && !mState.isCreated();
    }

    /** The attached view whose surface has this token, or null: an event for an older surface is dropped. */
    private static SurfaceView viewOf(long token) {
        for (SurfaceView view : sAttached) {
            if (token != 0 && view.mState.token() == token) {
                return view;
            }
        }
        return null;
    }

    /** @hide ActivityThread's, on the main thread: the session's SURFACE_CREATED (ADR-0043 c4). */
    public static void dispatchSurfaceCreated(long token, long window, int width, int height) {
        SurfaceView view = viewOf(token);
        if (view != null) {
            view.mState.onCreated(token, window, width, height);
        }
    }

    /** @hide ActivityThread's, on the main thread: the session's SURFACE_RESIZED. */
    public static void dispatchSurfaceResized(long token, int width, int height) {
        SurfaceView view = viewOf(token);
        if (view != null) {
            view.mState.onResized(token, width, height);
        }
    }

    /** @hide ActivityThread's, on the main thread: the session's SURFACE_DESTROYED, the host took the surface. */
    public static void dispatchSurfaceDestroyed(long token) {
        SurfaceView view = viewOf(token);
        if (view != null) {
            view.mState.onDestroyed(token);
        }
    }

    private final SurfaceHolder mSurfaceHolder = new SurfaceHolder() {
        private static final String LOG_TAG = "SurfaceHolder";

        @Override
        public boolean isCreating() {
            return mIsCreating;
        }

        @Override
        public void addCallback(Callback callback) {
            synchronized (mCallbacks) {
                if (!mCallbacks.contains(callback)) {
                    mCallbacks.add(callback);
                }
            }
        }

        @Override
        public void removeCallback(Callback callback) {
            synchronized (mCallbacks) {
                mCallbacks.remove(callback);
            }
        }

        @Override
        public void setFixedSize(int width, int height) {
            if (mRequestedWidth != width || mRequestedHeight != height) {
                mRequestedWidth = width;
                mRequestedHeight = height;
                requestLayout();
            }
        }

        @Override
        public void setSizeFromLayout() {
            if (mRequestedWidth != -1 || mRequestedHeight != -1) {
                mRequestedWidth = mRequestedHeight = -1;
                requestLayout();
            }
        }

        @Override
        public void setFormat(int format) {
            // As AOSP's: for backward compatibility, OPAQUE means 565 for SurfaceView.
            if (format == PixelFormat.OPAQUE) {
                format = PixelFormat.RGB_565;
            }
            mRequestedFormat = format;
            if (mState.isCreated()) {
                updateSurface();
            }
        }

        @Override
        @Deprecated
        public void setType(int type) {
        }

        @Override
        public void setKeepScreenOn(boolean screenOn) {
            post(() -> SurfaceView.this.setKeepScreenOn(screenOn));
        }

        @Override
        public Canvas lockCanvas() {
            return internalLockCanvas(null, false);
        }

        @Override
        public Canvas lockCanvas(Rect inOutDirty) {
            return internalLockCanvas(inOutDirty, false);
        }

        @Override
        public Canvas lockHardwareCanvas() {
            return internalLockCanvas(null, true);
        }

        /** As AOSP's: the surface's canvas, or null at most every 100 ms while there is none. */
        private Canvas internalLockCanvas(Rect dirty, boolean hardware) {
            mSurfaceLock.lock();
            Canvas canvas = null;
            if (mState.isCreated() && mSurface.isValid()) {
                try {
                    canvas = hardware ? mSurface.lockHardwareCanvas() : mSurface.lockCanvas(dirty);
                } catch (Exception e) {
                    Log.e(LOG_TAG, "Exception locking surface", e);
                }
            }
            if (canvas != null) {
                mLastLockTime = SystemClock.uptimeMillis();
                return canvas;
            }
            long now = SystemClock.uptimeMillis();
            long nextTime = mLastLockTime + 100;
            if (nextTime > now) {
                try {
                    Thread.sleep(nextTime - now);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                now = SystemClock.uptimeMillis();
            }
            mLastLockTime = now;
            mSurfaceLock.unlock();
            return null;
        }

        @Override
        public void unlockCanvasAndPost(Canvas canvas) {
            try {
                mSurface.unlockCanvasAndPost(canvas);
            } finally {
                mSurfaceLock.unlock();
            }
        }

        @Override
        public Surface getSurface() {
            return mSurface;
        }

        @Override
        public Rect getSurfaceFrame() {
            return mSurfaceFrame;
        }
    };
}
