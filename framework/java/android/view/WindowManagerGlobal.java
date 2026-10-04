/*
 * Our WindowManagerGlobal, under AOSP's name (ADR-0041 decision 2). Step c2c2 gives it the window manager's local
 * stub (ADR-0045 decision 3), which ViewConfiguration's constructor asks whether the display has a navigation bar; it
 * is made here until our ServiceManager's table holds "window" (ADR-0045 decision 2). Step c2c3 extends this file with
 * the one window (getInstance, addView, updateViewLayout, removeView and the roots) and getWindowSession, which must
 * leave the animators' scale at 1, since the stub's default would turn them off (ADR-0045 decision 2).
 */
package android.view;

import android.util.Log;
import com.android.minato.services.ServiceStub;

public final class WindowManagerGlobal {
    private static final String TAG = "WindowManagerGlobal";
    private static IWindowManager sWindowManagerService;

    private WindowManagerGlobal() {
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
