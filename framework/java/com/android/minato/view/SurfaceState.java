package com.android.minato.view;

import java.util.concurrent.atomic.AtomicLong;

/**
 * Our SurfaceView's state (ADR-0043 step c4), in plain Java so that the host's JVM tests it against a fake session:
 * what the view asks the session for, and which of SurfaceHolder's callbacks it gives, from its inputs and the
 * session's events. As AOSP's SurfaceView (SurfaceView.java at android-15.0.0_r1):
 * <ul>
 *   <li>a surface exists while the view is attached and visible, in a visible window that is not stopped, with a
 *       frame that is not empty (updateRequestedVisibility, :411-413); ours also waits for the window's own surface;
 *   <li>a new surface gives surfaceCreated, surfaceChanged and surfaceRedrawNeeded, a new size surfaceChanged and
 *       surfaceRedrawNeeded (:1240-1266), and a lost one surfaceDestroyed before it goes (:1220-1225);
 *   <li>a new format destroys the surface and makes another (updateSurface's formatChanged).
 * </ul>
 * Unlike AOSP's, a surface comes later: the session answers a make with an event (the plan's decision 2), and the
 * callbacks come with it. Each make takes a new token, so an answer for an older surface is dropped. When the host
 * takes a surface away (at the background), the view makes no other until the window's own surface comes back. A
 * host without surfaces is told once, and the view then has none.
 */
public final class SurfaceState {
    /** The session's surface requests (APKRT_REQUEST_SURFACE). A negative answer is an errno. */
    public interface Session {
        /** A new surface at a frame in window pixels, below the window (z < 0) or above it; size 0x0 = the frame's. */
        int make(long token, int z, int left, int top, int right, int bottom, int width, int height, int format);

        /** A new frame, z order or size for a made surface; only a new size is answered (onResized). */
        int place(long token, int z, int left, int top, int right, int bottom, int width, int height);

        /** The surface goes; no answer. */
        int remove(long token);
    }

    /** What the view does on the state's word. */
    public interface Callbacks {
        /** Make the Surface on the host's window, then surfaceCreated. */
        void created(long window, int width, int height);

        /** surfaceChanged with the holder's format. */
        void changed(int format, int width, int height);

        /** surfaceRedrawNeeded(Async). */
        void redrawNeeded();

        /** surfaceDestroyed, then release the Surface. */
        void destroyed();

        /** The first make failed: the host has no surfaces (EOPNOTSUPP) or refused this one. */
        void unsupported(int error);
    }

    /** Tokens of the process's surfaces, never 0 and never reused. */
    private static final AtomicLong sTokens = new AtomicLong();

    private final Session mSession;
    private final Callbacks mCallbacks;

    private boolean mAttached;
    private boolean mViewVisible;
    private boolean mWindowVisible;
    private boolean mWindowStopped;
    private boolean mWindowSurface;
    private int mLeft, mTop, mRight, mBottom;
    private int mFixedWidth, mFixedHeight;
    private int mFormat;
    private int mZ = -2;

    /** The surface asked for, 0 for none, and what was sent for it. */
    private long mToken;
    private int mSentLeft, mSentTop, mSentRight, mSentBottom;
    private int mSentWidth, mSentHeight;
    private int mSentFormat;
    private int mSentZ;

    private boolean mCreated;
    private long mWindow;
    private int mWidth, mHeight;
    /** The host took the surface away; none until the window's own surface comes back. */
    private boolean mTakenAway;
    private boolean mUnsupported;

    public SurfaceState(Session session, Callbacks callbacks) {
        mSession = session;
        mCallbacks = callbacks;
    }

    public void setAttached(boolean attached) {
        mAttached = attached;
    }

    public void setViewVisible(boolean visible) {
        mViewVisible = visible;
    }

    public void setWindowVisible(boolean visible) {
        mWindowVisible = visible;
    }

    public void setWindowStopped(boolean stopped) {
        mWindowStopped = stopped;
    }

    /** The window's own surface (its ViewRootImpl's host window). */
    public void setWindowSurface(boolean present) {
        if (!present) {
            mTakenAway = false;
        }
        mWindowSurface = present;
    }

    public void setFrame(int left, int top, int right, int bottom) {
        mLeft = left;
        mTop = top;
        mRight = right;
        mBottom = bottom;
    }

    /** The buffer's size (setFixedSize), or 0x0 for the frame's (setSizeFromLayout). */
    public void setFixedSize(int width, int height) {
        mFixedWidth = width;
        mFixedHeight = height;
    }

    /** The holder's format, OPAQUE already mapped to RGB_565 as AOSP's holder maps it. */
    public void setFormat(int format) {
        mFormat = format;
    }

    /** Android's sublayer: -2 media (the default), -1 media overlay, 1 on top. */
    public void setZ(int z) {
        mZ = z;
    }

    /** Where AOSP's SurfaceView runs updateSurface: asks the session for what the inputs say. */
    public void update() {
        boolean wanted = mAttached && mViewVisible && mWindowVisible && !mWindowStopped && mWindowSurface
                && !mTakenAway && !mUnsupported && mRight > mLeft && mBottom > mTop;
        if (!wanted) {
            drop();
            return;
        }
        if (mToken != 0 && mFormat != mSentFormat) {
            drop();
        }
        if (mToken == 0) {
            mToken = sTokens.incrementAndGet();
            sent();
            int answer = mSession.make(mToken, mZ, mLeft, mTop, mRight, mBottom, mFixedWidth, mFixedHeight, mFormat);
            if (answer < 0) {
                mToken = 0;
                mUnsupported = true;
                mCallbacks.unsupported(answer);
            }
            return;
        }
        if (mZ != mSentZ || mLeft != mSentLeft || mTop != mSentTop || mRight != mSentRight || mBottom != mSentBottom
                || mFixedWidth != mSentWidth || mFixedHeight != mSentHeight) {
            sent();
            mSession.place(mToken, mZ, mLeft, mTop, mRight, mBottom, mFixedWidth, mFixedHeight);
        }
    }

    private void sent() {
        mSentZ = mZ;
        mSentLeft = mLeft;
        mSentTop = mTop;
        mSentRight = mRight;
        mSentBottom = mBottom;
        mSentWidth = mFixedWidth;
        mSentHeight = mFixedHeight;
        mSentFormat = mFormat;
    }

    /** surfaceDestroyed before the surface goes, as Android calls it. */
    private void drop() {
        if (mToken == 0) {
            return;
        }
        if (mCreated) {
            mCreated = false;
            mWindow = 0;
            mCallbacks.destroyed();
        }
        mSession.remove(mToken);
        mToken = 0;
    }

    /** The session's answer to a make: the surface's window and size. */
    public void onCreated(long token, long window, int width, int height) {
        if (token == 0 || token != mToken || mCreated) {
            return;
        }
        mCreated = true;
        mWindow = window;
        mWidth = width;
        mHeight = height;
        mCallbacks.created(window, width, height);
        mCallbacks.changed(mSentFormat, width, height);
        mCallbacks.redrawNeeded();
    }

    /** The session's answer to a place with a new size. */
    public void onResized(long token, int width, int height) {
        if (token == 0 || token != mToken || !mCreated) {
            return;
        }
        mWidth = width;
        mHeight = height;
        mCallbacks.changed(mSentFormat, width, height);
        mCallbacks.redrawNeeded();
    }

    /** The host took the surface away: surfaceDestroyed, and the remove that frees its token. */
    public void onDestroyed(long token) {
        if (token == 0 || token != mToken) {
            return;
        }
        drop();
        mTakenAway = true;
    }

    public boolean isCreated() {
        return mCreated;
    }

    /** The token of the surface asked for, 0 for none. */
    public long token() {
        return mToken;
    }

    public long window() {
        return mWindow;
    }

    public int width() {
        return mWidth;
    }

    public int height() {
        return mHeight;
    }

    public int format() {
        return mSentFormat;
    }
}
