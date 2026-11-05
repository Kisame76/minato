package com.android.minato.view;

/**
 * Android 15's back gesture of gesture navigation, as SystemUI's edge back handler gives it, for a window of Minato,
 * which has no SystemUI (ADR-0041 decision 10 step b2): plain Java, so tools/framework-host-check.sh runs it. A touch
 * whose down lies within INSET_DP of the window's left or right edge and outside the window's system gesture
 * exclusion rects is a candidate: the app still gets it, until the touch has gone more than START_DP away from the
 * edge, more across than down; then the back starts, the app gets a cancel, and the touch is the back's until its up,
 * which commits it at COMMIT_DP or more from the down and cancels it nearer. A touch that first goes START_DP down or
 * up, a second finger, or a down elsewhere stays the app's. While the window hides the system bars to be shown by a
 * swipe, the first swipe that would start a back is the app's and shows them for TRANSIENT_MS, in which an edge swipe
 * is a back; a back hides them again. The values are those measured on apk-runner-35 with edge-back-test
 * (fixtures/expected/edge-back-test*.logcat).
 */
public final class EdgeBackGesture {
    /** The touch goes to the app as it is. */
    public static final int APP = 0;
    /** The back starts with this move: the app gets a cancel in its place. */
    public static final int START = 1;
    /** The back goes on with this move, which the app does not get. */
    public static final int PROGRESS = 2;
    /** The up commits the back, which the app does not get. */
    public static final int COMMIT = 3;
    /** The back ends without its commit; the app gets nothing of the touch any more. */
    public static final int CANCEL = 4;
    /** The rest of a touch whose back was cancelled: the app does not get it. */
    public static final int SWALLOW = 5;

    /** Measured: a down at 29 dp from the edge starts a back, one at 30 dp does not. */
    static final float INSET_DP = 30;
    /** Measured: a swipe of 18 dp from the edge starts a back, one of 16 dp does not. */
    static final float START_DP = 17;
    /** Measured: an up 20 dp from the down commits the back, one 18 dp from it cancels it. */
    static final float COMMIT_DP = 19;
    /**
     * Measured: with hidden bars, an edge swipe about 2.0 s after the one that showed them is a back, one about 2.5 s
     * after not, as SystemUI hides transient bars again after 2250 ms.
     */
    static final long TRANSIENT_MS = 2250;

    private static final int IDLE = 0, CANDIDATE = 1, STARTED = 2, ENDED = 3, APPS = 4;

    private final float mInset;
    private final float mSlop;
    private final float mCommit;
    private boolean mBarsHidden;
    private long mTime;
    private long mBarsShownUntil;
    private float mWidth;
    private int[] mExclusions = new int[0];
    private int mState = IDLE;
    private float mDownX;
    private float mDownY;
    private boolean mLeft;

    public EdgeBackGesture(float density) {
        mInset = INSET_DP * density;
        mSlop = START_DP * density;
        mCommit = COMMIT_DP * density;
    }

    /** Whether the window hides the system bars to be shown by a swipe (BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE). */
    public void setBarsHidden(boolean hidden) {
        mBarsHidden = hidden;
    }

    /** The uptime in ms of the event that comes next. */
    public void setTime(long time) {
        mTime = time;
    }

    /** The window's size and its exclusion rects, four ints each (left, top, right, bottom), in its pixels. */
    public void setWindow(float width, float height, int[] exclusions) {
        mWidth = width;
        mExclusions = exclusions.clone();
    }

    /** Whether the back has started and not ended yet: the window's back callbacks have it. */
    public boolean inProgress() {
        return mState == STARTED;
    }

    /** Whether the touch is the back's: started, or cancelled and not over yet; the app gets none of it. */
    public boolean owns() {
        return mState == STARTED || mState == ENDED;
    }

    /** Which edge the back came from: true for the left one. */
    public boolean fromLeft() {
        return mLeft;
    }

    /** A touch's first finger goes down. */
    public int down(float x, float y) {
        mDownX = x;
        mDownY = y;
        mState = IDLE;
        if (x < mInset) {
            mLeft = true;
        } else if (mWidth - x < mInset) {
            mLeft = false;
        } else {
            mState = APPS;
            return APP;
        }
        mState = excluded(x, y) ? APPS : CANDIDATE;
        return APP;
    }

    /** The touch's finger moves. */
    public int move(float x, float y) {
        switch (mState) {
            case CANDIDATE:
                float across = mLeft ? x - mDownX : mDownX - x;
                float down = Math.abs(y - mDownY);
                if (down > mSlop && down >= across) {
                    mState = APPS;
                } else if (across > mSlop) {
                    if (mBarsHidden && mTime >= mBarsShownUntil) {
                        // The swipe shows the hidden bars for a while and stays the app's.
                        mBarsShownUntil = mTime + TRANSIENT_MS;
                        mState = APPS;
                        return APP;
                    }
                    mState = STARTED;
                    return START;
                }
                return APP;
            case STARTED:
                return PROGRESS;
            case ENDED:
                return SWALLOW;
            default:
                return APP;
        }
    }

    /** The touch's finger goes up. */
    public int up(float x, float y) {
        int state = mState;
        mState = IDLE;
        switch (state) {
            case STARTED:
                mBarsShownUntil = 0;
                return (mLeft ? x - mDownX : mDownX - x) >= mCommit ? COMMIT : CANCEL;
            case ENDED:
                return SWALLOW;
            default:
                return APP;
        }
    }

    /** A second finger goes down: a back that started is cancelled, a candidate is the app's. */
    public int pointerDown() {
        switch (mState) {
            case STARTED:
                mState = ENDED;
                mBarsShownUntil = 0;
                return CANCEL;
            case ENDED:
                return SWALLOW;
            default:
                mState = APPS;
                return APP;
        }
    }

    /** The touch was cancelled: a back that started is cancelled too. */
    public int cancel() {
        int state = mState;
        mState = IDLE;
        return state == STARTED ? CANCEL : state == ENDED ? SWALLOW : APP;
    }

    private boolean excluded(float x, float y) {
        for (int i = 0; i + 3 < mExclusions.length; i += 4) {
            if (x >= mExclusions[i] && x < mExclusions[i + 2] && y >= mExclusions[i + 1] && y < mExclusions[i + 3]) {
                return true;
            }
        }
        return false;
    }
}
