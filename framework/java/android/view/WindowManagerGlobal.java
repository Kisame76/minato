/*
 * Our WindowManagerGlobal, under AOSP's name (ADR-0041 decision 2). Step c2c2 gave it the window manager's local
 * stub (ADR-0045 decision 3), which ViewConfiguration's constructor asks whether the display has a navigation bar; it
 * is made here until our ServiceManager's table holds "window" (ADR-0045 decision 2). Step c2c3 adds the windows:
 *   - an activity's window (TYPE_BASE_APPLICATION) gets its ViewRootImpl, as AOSP's addView makes one, and goes with
 *     removeView;
 *   - an application window (a dialog) and a sub-window (a popup, a panel) get theirs as requested windows of the
 *     session's host (ADR-0041 gap 4 (b)), after the checks WMS's addWindow makes of their token, which throw
 *     Android's BadTokenException from setView: an application window needs an activity that is not destroyed, a
 *     sub-window a shown window that is no sub-window itself; TYPE_APPLICATION_OVERLAY is refused as without
 *     SYSTEM_ALERT_WINDOW. A window's sub-windows go before it, as WMS's removeImmediately takes the children;
 *   - the windows' input and focus (gap 4 (b2), decision 4): the session's touches go to the window Android 15's
 *     InputDispatcher picks among the shown windows in decision 1's order, and the session's focus to the topmost
 *     focusable one, which also gets the system's back (ADR-0041 decision 10);
 *   - every other window (a toast, a system window) is a gap that is logged and never shown, so that its
 *     updateViewLayout and removeView still answer (ADR-0041 decision 2, step 8);
 *   - no window session: getWindowSession is null, as Minato has no system_server, and ViewRootImpl lays out
 *     locally. AOSP's getWindowManagerService and getWindowSession set ValueAnimator's duration scale from the
 *     window manager; ours never do, so the animators keep a scale of 1, where the stub's default of 0 would turn
 *     them off.
 */
package android.view;

import android.app.ActivityThread;
import android.app.NotificationService;
import android.graphics.Rect;
import android.os.IBinder;
import android.util.Log;
import com.android.minato.services.ServiceStub;
import java.util.ArrayList;
import java.util.List;

public final class WindowManagerGlobal {
    private static final String TAG = "WindowManagerGlobal";
    private static IWindowManager sWindowManagerService;
    private static WindowManagerGlobal sDefaultWindowManager;

    private final Object mLock = new Object();
    /** The shown windows' views, roots and attributes, at the same index, as AOSP's three lists. */
    private final ArrayList<View> mViews = new ArrayList<>();
    private final ArrayList<ViewRootImpl> mRoots = new ArrayList<>();
    private final ArrayList<WindowManager.LayoutParams> mParams = new ArrayList<>();
    /** The windows that were added but are a gap, never shown. */
    private final ArrayList<View> mGapViews = new ArrayList<>();
    /** The session's window, the top activity's, and whether the session has the focus (ActivityThread's). */
    private ViewRootImpl mSessionRoot;
    private boolean mSessionFocus;
    /** The window that has the focus, and the one that took the open gesture's DOWN. */
    private ViewRootImpl mFocusedRoot;
    private ViewRootImpl mTouchedRoot;

    private WindowManagerGlobal() {
    }

    public static WindowManagerGlobal getInstance() {
        synchronized (WindowManagerGlobal.class) {
            if (sDefaultWindowManager == null) {
                sDefaultWindowManager = new WindowManagerGlobal();
            }
            return sDefaultWindowManager;
        }
    }

    /** The window manager, made once, as AOSP's keeps sWindowManagerService. */
    public static IWindowManager getWindowManagerService() {
        synchronized (WindowManagerGlobal.class) {
            if (sWindowManagerService == null) {
                sWindowManagerService = ServiceStub.create(IWindowManager.class, null, new Answers(),
                        (method, args) -> { }, line -> Log.i(TAG, line));
            }
            return sWindowManagerService;
        }
    }

    /** The process's window session: none, as no system_server lays out Minato's windows. */
    public static IWindowSession getWindowSession() {
        return null;
    }

    public static IWindowSession peekWindowSession() {
        return null;
    }

    /** As AOSP's addView for an activity's window; any other window is a logged gap. */
    @SuppressWarnings("deprecation") // TYPE_TOAST, which Toast's windows still have.
    public void addView(View view, ViewGroup.LayoutParams params, Display display, Window parentWindow, int userId) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        if (display == null) {
            throw new IllegalArgumentException("display must not be null");
        }
        if (!(params instanceof WindowManager.LayoutParams)) {
            throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        }
        final WindowManager.LayoutParams wparams = (WindowManager.LayoutParams) params;
        if (parentWindow != null) {
            parentWindow.adjustLayoutParamsForSubWindow(wparams);
        }
        ViewRootImpl root;
        synchronized (mLock) {
            if (mViews.contains(view) || mGapViews.contains(view)) {
                throw new IllegalStateException("View " + view + " has already been added to the window manager.");
            }
            final int type = wparams.type;
            final boolean application = type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                    && type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW;
            final boolean subWindow = type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                    && type <= WindowManager.LayoutParams.LAST_SUB_WINDOW;
            if (!application && !subWindow && type != WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    && type != WindowManager.LayoutParams.TYPE_TOAST) {
                Log.w(TAG, "Not shown: a window of type " + type + " (" + wparams.getTitle()
                        + "), as Minato shows only activities, dialogs, popups and toasts yet (ADR-0041 gap 4 (b))");
                view.setLayoutParams(wparams);
                mGapViews.add(view);
                return;
            }
            root = new ViewRootImpl(view.getContext(), display);
            view.setLayoutParams(wparams);
            mViews.add(view);
            mRoots.add(root);
            mParams.add(wparams);
            try {
                if (type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                    checkToken(root, wparams, application);
                }
                root.setView(view, wparams, null, userId);
            } catch (RuntimeException e) {
                removeViewLocked(mViews.size() - 1, true);
                throw e;
            }
        }
    }

    /**
     * As WMS's addWindow checks a window's token, with the answer ViewRootImpl.setView turns into Android's
     * BadTokenException [verified: second-windows-test on apk-runner-35]: TYPE_APPLICATION_OVERLAY is refused as
     * without SYSTEM_ALERT_WINDOW (ADD_PERMISSION_DENIED); an application window's token must be an activity's that
     * is not destroyed (ADD_BAD_APP_TOKEN); a sub-window's must be a shown window's that is no sub-window itself
     * (ADD_BAD_SUBWINDOW_TOKEN); a toast's must be a window token of the notification service's, as WMS's addWindow
     * refuses an app's own toast window from target O on (ADD_BAD_APP_TOKEN, gap 4 (b4)). The window then is a
     * requested one, an application window in its activity's group, taken away while its activity is not visible (gap
     * 4 (b3)), a toast above every group.
     */
    @SuppressWarnings("deprecation") // TYPE_TOAST, which Toast's windows still have.
    private void checkToken(ViewRootImpl root, WindowManager.LayoutParams wparams, boolean application) {
        if (wparams.type == WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY) {
            throw new WindowManager.BadTokenException("Unable to add window " + root.windowToken()
                    + " -- permission denied for window type " + wparams.type);
        }
        if (wparams.type == WindowManager.LayoutParams.TYPE_TOAST) {
            if (!NotificationService.isToastWindowToken(wparams.token)) {
                throw new WindowManager.BadTokenException("Unable to add window -- token " + wparams.token
                        + " is not valid; is your activity running?");
            }
            root.setRequested(null, 0);
            return;
        }
        if (application) {
            ActivityThread thread = ActivityThread.currentActivityThread();
            if (thread == null || !thread.isActivityTokenValid(wparams.token)) {
                throw new WindowManager.BadTokenException("Unable to add window -- token " + wparams.token
                        + " is not valid; is your activity running?");
            }
            final int group = thread.windowGroupOf(wparams.token);
            root.setRequested(null, Math.max(group, 0));
            if (group < 0) {
                root.withdrawRequest();
            }
            return;
        }
        ViewRootImpl parent = null;
        for (ViewRootImpl candidate : mRoots) {
            if (candidate != root && wparams.token != null && candidate.windowToken() == wparams.token) {
                parent = candidate;
                break;
            }
        }
        if (parent == null || parent.parentRoot() != null) {
            throw new WindowManager.BadTokenException("Unable to add window -- token " + wparams.token
                    + " is not valid; is your activity running?");
        }
        root.setRequested(parent, 0);
    }

    /**
     * @hide ActivityThread's, at the session's WINDOW_CREATED: the requested window of `token` gets its surface; false
     * when no window has the token, which may then be a SurfaceView's (decision 7).
     */
    public boolean dispatchWindowCreated(long token, long window, int width, int height) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null) {
            root.setHostWindow(window, width, height);
            updateFocus();
        }
        return root != null;
    }

    /** @hide ActivityThread's, at WINDOW_RESIZED: the requested window's new size; false as at the creation. */
    public boolean dispatchWindowResized(long token, int width, int height) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null && root.hasHostWindow()) {
            root.setHostWindow(root.hostWindow(), width, height);
        }
        return root != null;
    }

    /**
     * @hide ActivityThread's, at WINDOW_DESTROYED: the host took the window away at the background or the end; its
     * surface goes and its token is removed, and it is asked for again at the session's return (decision 6, gap 4
     * (b5a)). False as at the creation.
     */
    public boolean dispatchWindowDestroyed(long token) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null) {
            root.hostTookWindow();
            updateFocus();
        }
        return root != null;
    }

    /**
     * @hide ActivityThread's, at the session's RESUME after the background (decision 6, gap 4 (b5a)): the requested
     * windows the host took away are asked for again, each sub-window once its parent has its window, as the guest's
     * ViewRootImpls relayout at the start.
     */
    public void dispatchSessionReturn() {
        final ArrayList<ViewRootImpl> roots;
        synchronized (mLock) {
            roots = new ArrayList<>(mRoots);
        }
        for (ViewRootImpl root : roots) {
            root.hostGivesBack();
        }
    }

    /**
     * The soft keyboard's height in the session's pixels (ADR-0068 step 2d, APKRT_EVENT_IME_INSETS): the ime() inset
     * of every window, which asks its views to apply their insets again. {@code noVisibleFrame}: the keyboard gives no
     * visible frame, so the whole of it counts as covering the window, as Android's InsetsSource takes an input
     * method's window frame where its window reports no visible insets.
     */
    public void dispatchImeInsets(int bottom, boolean noVisibleFrame) {
        final ArrayList<ViewRootImpl> roots;
        synchronized (mLock) {
            ViewRootImpl.sImeBottom = Math.max(bottom, 0);
            ViewRootImpl.sImeNoVisibleFrame = noVisibleFrame;
            roots = new ArrayList<>(mRoots);
        }
        for (ViewRootImpl root : roots) {
            root.imeInsetsChanged();
        }
    }

    /**
     * @hide ActivityThread's: the session's back (ADR-0041 decision 10, APKRT_EVENT_BACK) with its phase, its progress
     * in 1/10000 and its edge, for the window that has the focus, as Android 15's window manager gives a back to the
     * focused window's callback. Without one it goes nowhere.
     */
    public void dispatchBack(int phase, int progress, int edge) {
        final ViewRootImpl focused;
        synchronized (mLock) {
            focused = mFocusedRoot;
        }
        if (focused == null) {
            Log.w(TAG, "The session's back finds no window with the focus");
            return;
        }
        focused.dispatchSystemBack(phase, Math.min(Math.max(progress, 0), 10000) / 10000f, edge);
    }

    /**
     * @hide ActivityThread's: the user hid the keyboard (ADR-0068, APKRT_EVENT_IME_HIDDEN), for the window that has the
     * focus, as Android's input method service hides itself on BACK. Without a window with the focus it goes nowhere.
     */
    public void dispatchImeHidden() {
        final ViewRootImpl focused;
        synchronized (mLock) {
            focused = mFocusedRoot;
        }
        if (focused == null) {
            Log.w(TAG, "The keyboard's hide finds no window with the focus");
            return;
        }
        focused.dispatchImeHiddenByUser();
    }

    /** ViewRootImpl's, once `parent` has its window: its sub-windows that waited for it ask for theirs (gap 4 (b3)). */
    void childrenMayAsk(ViewRootImpl parent) {
        final ArrayList<ViewRootImpl> children = new ArrayList<>();
        synchronized (mLock) {
            for (ViewRootImpl root : mRoots) {
                if (root.parentRoot() == parent) {
                    children.add(root);
                }
            }
        }
        for (ViewRootImpl child : children) {
            child.parentReady();
        }
    }

    /**
     * @hide ActivityThread's (gap 4 (b3), decision 3): an activity whose window changed, as ActivityWindows.update
     * gave it. First every window of each activity whose window changed goes, top down, as WMS hides an activity that
     * is not visible: its windows' sub-windows, then its dialogs and other application windows, then its own window's
     * request; then ActivityThread moves the session's window; then each one that is visible gets its windows again,
     * bottom up: its own window on the session's window or as a requested one in its group, its application windows in
     * that group, and their sub-windows once their parents have their windows.
     */
    public void withdrawActivityWindows(IBinder activity) {
        for (ViewRootImpl root : activityWindows(activity)) {
            for (ViewRootImpl child : childrenOf(root)) {
                child.withdrawRequest();
            }
        }
        for (ViewRootImpl root : activityWindows(activity)) {
            if (root.mWindowAttributes.type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                root.withdrawRequest();
            }
        }
        for (ViewRootImpl root : activityWindows(activity)) {
            if (root.mWindowAttributes.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                root.withdrawRequest();
            }
        }
    }

    /**
     * @hide ActivityThread's, after withdrawActivityWindows and the session's window's move: the visible activity's
     * windows again, its own on the session's window or, `requested`, as a requested one, all in group `group`.
     */
    public void restoreActivityWindows(IBinder activity, boolean requested, int group) {
        for (ViewRootImpl root : activityWindows(activity)) {
            if (root.mWindowAttributes.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                if (requested) {
                    root.requestActivityWindow(group);
                } else {
                    root.useSessionWindow();
                }
            }
        }
        for (ViewRootImpl root : activityWindows(activity)) {
            if (root.mWindowAttributes.type != WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
                root.restoreRequest(group);
            }
            for (ViewRootImpl child : childrenOf(root)) {
                child.restoreRequest(0);
            }
        }
    }

    /** The activity's application windows, its own first, in the order of their adds. */
    private ArrayList<ViewRootImpl> activityWindows(IBinder activity) {
        final ArrayList<ViewRootImpl> windows = new ArrayList<>();
        synchronized (mLock) {
            for (int i = 0; i < mRoots.size(); i++) {
                final WindowManager.LayoutParams params = mParams.get(i);
                if (activity != null && params.token == activity && mRoots.get(i).parentRoot() == null
                        && params.type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                        && params.type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW) {
                    windows.add(mRoots.get(i));
                }
            }
        }
        return windows;
    }

    /** A window's sub-windows, in the order of their adds. */
    private ArrayList<ViewRootImpl> childrenOf(ViewRootImpl parent) {
        final ArrayList<ViewRootImpl> children = new ArrayList<>();
        synchronized (mLock) {
            for (ViewRootImpl root : mRoots) {
                if (root.parentRoot() == parent) {
                    children.add(root);
                }
            }
        }
        return children;
    }

    private ViewRootImpl rootOfRequest(long token) {
        synchronized (mLock) {
            for (ViewRootImpl root : mRoots) {
                if (root.requestToken() == token) {
                    return root;
                }
            }
        }
        return null;
    }

    public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        if (!(params instanceof WindowManager.LayoutParams)) {
            throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        }
        final WindowManager.LayoutParams wparams = (WindowManager.LayoutParams) params;
        view.setLayoutParams(wparams);
        synchronized (mLock) {
            if (mGapViews.contains(view)) {
                return;
            }
            int index = findViewLocked(view, true);
            ViewRootImpl root = mRoots.get(index);
            mParams.set(index, wparams);
            root.setLayoutParams(wparams, false);
        }
        updateFocus();
    }

    public void removeView(View view, boolean immediate) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        synchronized (mLock) {
            if (mGapViews.remove(view)) {
                return;
            }
            int index = findViewLocked(view, true);
            View curView = mRoots.get(index).getView();
            removeViewLocked(index, immediate);
            if (curView == view) {
                return;
            }
            throw new IllegalStateException("Calling with view " + view + " but the ViewAncestor is attached to "
                    + curView);
        }
    }

    /** As AOSP's: every window of the token goes, logged as leaked when the caller names itself. */
    public void closeAll(IBinder token, String who, String what) {
        synchronized (mLock) {
            for (int i = mViews.size() - 1; i >= 0; i--) {
                if (token == null || mParams.get(i).token == token) {
                    if (who != null) {
                        Log.e(TAG, what + " " + who + " has leaked window " + mRoots.get(i).getView()
                                + " that was originally added here");
                    }
                    removeViewLocked(i, false);
                }
            }
        }
    }

    private void removeViewLocked(int index, boolean immediate) {
        ViewRootImpl root = mRoots.get(index);
        // As WMS's removeImmediately: the window's sub-windows go before it.
        for (int i = mRoots.size() - 1; i >= 0; i--) {
            if (mRoots.get(i).parentRoot() == root) {
                ViewRootImpl child = mRoots.get(i);
                View childView = child.getView();
                child.die(true);
                if (childView != null) {
                    childView.assignParent(null);
                }
            }
        }
        View view = root.getView();
        root.die(immediate);
        if (view != null) {
            view.assignParent(null);
        }
    }

    /** ViewRootImpl's call once its window has died: the window leaves the lists. */
    void doRemoveView(ViewRootImpl root) {
        synchronized (mLock) {
            final int index = mRoots.indexOf(root);
            if (index >= 0) {
                mRoots.remove(index);
                mParams.remove(index);
                mViews.remove(index);
            }
        }
        if (mTouchedRoot == root) {
            mTouchedRoot = null;
        }
        updateFocus();
    }

    /**
     * @hide ActivityThread's: the session's window (the top activity's, or null) and its focus, which goes to the
     * topmost focusable window (decision 4): a dialog takes it from its activity and gives it back at its end.
     */
    public void setSessionWindow(ViewRootImpl root, boolean focus) {
        mSessionRoot = root;
        mSessionFocus = focus;
        updateFocus();
    }

    /**
     * @hide ActivityThread's, as the session's window goes with its activity: no window has the focus or the gesture,
     * and none hears of it, as Android's removed window gets no focus event.
     */
    public void forgetSessionWindow(ViewRootImpl root) {
        if (mSessionRoot == root) {
            mSessionRoot = null;
            mFocusedRoot = null;
            mTouchedRoot = null;
        }
    }

    /**
     * As Android 15's focus (DisplayContent.findFocusedWindow, InputDispatcher's focus events): the topmost shown
     * window that is focusable while the session has the focus, none without it; the window that loses it hears of it
     * before the one that gains it.
     */
    private void updateFocus() {
        ViewRootImpl focused = null;
        if (mSessionFocus) {
            for (ViewRootImpl root : windowsTopFirst(true)) {
                if ((root.mWindowAttributes.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0) {
                    focused = root;
                    break;
                }
            }
        }
        if (focused == mFocusedRoot) {
            return;
        }
        final ViewRootImpl lost = mFocusedRoot;
        mFocusedRoot = focused;
        if (lost != null) {
            lost.windowFocusChanged(false);
        }
        if (focused != null) {
            focused.windowFocusChanged(true);
        }
    }

    /**
     * @hide The session's receiver's, for a key on the session's channel: the window that has the focus, as Android
     * 15's InputDispatcher gives a key to the focused window; the session's own window without one.
     */
    ViewRootImpl routeKey(ViewRootImpl session) {
        if (session != mSessionRoot) {
            return session;
        }
        final ViewRootImpl focused = mFocusedRoot;
        return focused != null ? focused : session;
    }

    /**
     * @hide The session's receiver's, for a touch on the session's channel, in the display's pixels: the window that
     * takes it, as Android 15's InputDispatcher.findTouchedWindowTargetsLocked picks it at a gesture's DOWN, the
     * topmost shown touchable window that holds the point or is touch-modal (focusable without
     * FLAG_NOT_TOUCH_MODAL), the windows above it that watch outside touches getting ACTION_OUTSIDE; the gesture's
     * other events go to the same window, none once it went. The session's window takes the event as it is; another
     * gets a copy in its own pixels. Null drops it. Left out: split touches, as one window takes a whole gesture.
     */
    ViewRootImpl routeTouch(ViewRootImpl session, MotionEvent event) {
        if (session != mSessionRoot) {
            return session;
        }
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            mTouchedRoot = null;
            final ArrayList<ViewRootImpl> outside = new ArrayList<>();
            // The session's window lies at the display's origin, so its pixels are the display's.
            final int x = (int) event.getX();
            final int y = (int) event.getY();
            for (ViewRootImpl root : windowsTopFirst(false)) {
                final int flags = root.mWindowAttributes.flags;
                if ((flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) {
                    continue;
                }
                final boolean touchModal = (flags & (WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)) == 0;
                if (root.mWinFrame.contains(x, y) || touchModal) {
                    mTouchedRoot = root;
                    break;
                }
                if ((flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
                    outside.add(root);
                }
            }
            for (ViewRootImpl root : outside) {
                final MotionEvent copy = copyFor(root, event);
                copy.setAction(MotionEvent.ACTION_OUTSIDE);
                root.deliverRouted(copy);
            }
        }
        final ViewRootImpl target = mTouchedRoot;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            mTouchedRoot = null;
        }
        if (target != null && target != session) {
            target.deliverRouted(copyFor(target, event));
        } else if (target != null && session.requestToken() != 0) {
            // The input's window is a requested one at its frame, a floating activity's at the bottom (gap 4 (b3)).
            event.offsetLocation(-session.mWinFrame.left, -session.mWinFrame.top);
        }
        return target;
    }

    /**
     * @hide ActivityThread's at an activity switch: a gesture that a window above the session's took ends there with a
     * CANCEL, as the session's own window's does (step b3.5b2-1b).
     */
    public void cancelRoutedTouch() {
        final ViewRootImpl touched = mTouchedRoot;
        mTouchedRoot = null;
        if (touched != null && touched != mSessionRoot) {
            touched.cancelTouch();
        }
    }

    private static MotionEvent copyFor(ViewRootImpl root, MotionEvent event) {
        final MotionEvent copy = MotionEvent.obtain(event);
        copy.offsetLocation(-root.mWinFrame.left, -root.mWinFrame.top);
        return copy;
    }

    /**
     * The shown windows, topmost first, in decision 1's order as APKSession's WindowOrder has it: the session's
     * window, with its sub-windows, then the requested application windows by group and newer above, each with its
     * sub-windows around it by sublayer (below it for a negative one) and newer above. A requested window is shown
     * once the host made its window, and while its view is visible; for the `focus`, one that waits for its window too
     * (gap 4 (b5a)).
     */
    private ArrayList<ViewRootImpl> windowsTopFirst(boolean focus) {
        final ArrayList<ViewRootImpl> order = new ArrayList<>();
        if (mSessionRoot == null) {
            return order;
        }
        final ArrayList<ViewRootImpl> shown = new ArrayList<>();
        synchronized (mLock) {
            for (ViewRootImpl root : mRoots) {
                if (root.requestToken() != 0 && (root.hasHostWindow() || (focus && root.isRequestPending()))
                        && root.getView() != null
                        && root.getView().getVisibility() == View.VISIBLE) {
                    shown.add(root);
                }
            }
        }
        // Below a floating activity at the bottom the session's window is empty, and the session's input reaches that
        // activity's requested window (gap 4 (b3)).
        if (mSessionRoot.requestToken() == 0) {
            place(mSessionRoot, shown, order);
        }
        final ArrayList<ViewRootImpl> applications = new ArrayList<>();
        for (ViewRootImpl root : shown) {
            if (root.parentRoot() == null) {
                applications.add(root);
            }
        }
        applications.sort((a, b) -> Integer.compare(a.requestGroup(), b.requestGroup()));
        for (ViewRootImpl root : applications) {
            place(root, shown, order);
        }
        java.util.Collections.reverse(order);
        return order;
    }

    private static void place(ViewRootImpl window, ArrayList<ViewRootImpl> shown, ArrayList<ViewRootImpl> order) {
        final ArrayList<ViewRootImpl> children = new ArrayList<>();
        for (ViewRootImpl root : shown) {
            if (root.parentRoot() == window) {
                children.add(root);
            }
        }
        children.sort((a, b) -> Integer.compare(sublayer(a), sublayer(b)));
        for (ViewRootImpl child : children) {
            if (sublayer(child) < 0) {
                place(child, shown, order);
            }
        }
        order.add(window);
        for (ViewRootImpl child : children) {
            if (sublayer(child) >= 0) {
                place(child, shown, order);
            }
        }
    }

    /** A sub-window's sublayer, as WindowManagerPolicy.getSubWindowLayerFromTypeLw (WindowOrder.sublayer). */
    private static int sublayer(ViewRootImpl root) {
        switch (root.mWindowAttributes.type) {
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA:
                return -2;
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA_OVERLAY:
                return -1;
            case WindowManager.LayoutParams.TYPE_APPLICATION_PANEL:
            case WindowManager.LayoutParams.TYPE_APPLICATION_ATTACHED_DIALOG:
                return 1;
            case WindowManager.LayoutParams.TYPE_APPLICATION_SUB_PANEL:
                return 2;
            case WindowManager.LayoutParams.TYPE_APPLICATION_ABOVE_SUB_PANEL:
                return 3;
            default:
                return 0;
        }
    }

    private int findViewLocked(View view, boolean required) {
        final int index = mViews.indexOf(view);
        if (required && index < 0) {
            throw new IllegalArgumentException("View=" + view + " not attached to window manager");
        }
        return index;
    }

    /** The roots of the token's windows, as AOSP's for the activity transitions and Activity.dump. */
    public ArrayList<ViewRootImpl> getRootViews(IBinder token) {
        ArrayList<ViewRootImpl> views = new ArrayList<>();
        synchronized (mLock) {
            for (int i = 0; i < mRoots.size(); i++) {
                if (mParams.get(i).token == token) {
                    views.add(mRoots.get(i));
                }
            }
        }
        return views;
    }

    /** As AOSP's: the views of the shown windows, which WindowInspector.getGlobalWindowViews gives. */
    public List<View> getWindowViews() {
        synchronized (mLock) {
            return new ArrayList<>(mViews);
        }
    }

    /** As AOSP's: the shown window whose view has this window token, which PopupWindow asks for its anchor's. */
    public View getWindowView(IBinder windowToken) {
        synchronized (mLock) {
            final int numViews = mViews.size();
            for (int i = 0; i < numViews; ++i) {
                final View view = mViews.get(i);
                if (view.getWindowToken() == windowToken) {
                    return view;
                }
            }
        }
        return null;
    }

    /** As AOSP's for the main thread's windows: an activity's stop and restart reach its windows. */
    public void setStoppedState(IBinder token, boolean stopped) {
        ArrayList<ViewRootImpl> roots = new ArrayList<>();
        synchronized (mLock) {
            for (int i = mViews.size() - 1; i >= 0; i--) {
                if (token == null || mParams.get(i).token == token) {
                    roots.add(mRoots.get(i));
                }
            }
        }
        for (ViewRootImpl root : roots) {
            root.setWindowStopped(stopped);
        }
    }

    /** As AOSP's: the token's window takes the opacity, which only the GPU's canvas uses (M4 step 10). */
    public void changeCanvasOpacity(IBinder token, boolean opaque) {
        if (token == null) {
            return;
        }
        synchronized (mLock) {
            for (int i = mParams.size() - 1; i >= 0; --i) {
                if (mParams.get(i).token == token) {
                    mRoots.get(i).changeCanvasOpacity(opaque);
                    return;
                }
            }
        }
    }

    /** The stub's answers; every other method logs once and returns its type's default. */
    static final class Answers {
        /**
         * An iPhone has no menu key: like a phone with gesture navigation, the display has a navigation bar, so
         * ViewConfiguration finds no permanent menu key and apps show their overflow menus. The reference AVDs keep
         * the emulator's default hardware keys and answer the opposite, so view-tree-test only shows that line.
         */
        public boolean hasNavigationBar(int displayId) {
            return true;
        }
    }
}
