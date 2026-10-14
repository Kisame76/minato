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
 *   - every other window (a toast, a system window) is a gap that is logged and never shown, so that its
 *     updateViewLayout and removeView still answer (ADR-0041 decision 2, step 8);
 *   - no window session: getWindowSession is null, as Minato has no system_server, and ViewRootImpl lays out
 *     locally. AOSP's getWindowManagerService and getWindowSession set ValueAnimator's duration scale from the
 *     window manager; ours never do, so the animators keep a scale of 1, where the stub's default of 0 would turn
 *     them off.
 */
package android.view;

import android.app.ActivityThread;
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
            if (!application && !subWindow && type != WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY) {
                Log.w(TAG, "Not shown: a window of type " + type + " (" + wparams.getTitle()
                        + "), as Minato shows only activities, dialogs and popups yet (ADR-0041 gap 4 (b))");
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
     * (ADD_BAD_SUBWINDOW_TOKEN). The window then is a requested one.
     */
    private void checkToken(ViewRootImpl root, WindowManager.LayoutParams wparams, boolean application) {
        if (wparams.type == WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY) {
            throw new WindowManager.BadTokenException("Unable to add window " + root.windowToken()
                    + " -- permission denied for window type " + wparams.type);
        }
        if (application) {
            ActivityThread thread = ActivityThread.currentActivityThread();
            if (thread == null || !thread.isActivityTokenValid(wparams.token)) {
                throw new WindowManager.BadTokenException("Unable to add window -- token " + wparams.token
                        + " is not valid; is your activity running?");
            }
            root.setRequested(null, 0);
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

    /** @hide ActivityThread's, at the session's WINDOW_CREATED: the requested window of `token` gets its surface. */
    public void dispatchWindowCreated(long token, long window, int width, int height) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null) {
            root.setHostWindow(window, width, height);
        }
    }

    /** @hide ActivityThread's, at WINDOW_RESIZED: the requested window's new size. */
    public void dispatchWindowResized(long token, int width, int height) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null && root.hasHostWindow()) {
            root.setHostWindow(root.hostWindow(), width, height);
        }
    }

    /** @hide ActivityThread's, at WINDOW_DESTROYED: the host took the window away; its surface goes. */
    public void dispatchWindowDestroyed(long token) {
        ViewRootImpl root = rootOfRequest(token);
        if (root != null) {
            root.setHostWindow(0, 0, 0);
        }
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
