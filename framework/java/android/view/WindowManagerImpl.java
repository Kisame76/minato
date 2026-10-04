/*
 * Our WindowManagerImpl, under AOSP's name (ADR-0041 decision 2, ADR-0039 step c2c3): what a Context's "window"
 * service gives, and what Window.setWindowManager wraps per window with createLocalWindowManager. As AOSP's, it hands
 * windows to WindowManagerGlobal with the context's display and the parent window, which gives a sub-window its
 * token. The window metrics (API 30), which AOSP's WindowMetricsController computes from the window manager's
 * frames, and the window manager's other calls are WindowManager's defaults until ADR-0041 a)'s insets.
 */
package android.view;

import android.content.Context;
import android.graphics.Region;
import android.os.IBinder;

public final class WindowManagerImpl implements WindowManager {
    private final WindowManagerGlobal mGlobal = WindowManagerGlobal.getInstance();
    public final Context mContext;
    private final Window mParentWindow;
    private IBinder mDefaultToken;

    public WindowManagerImpl(Context context) {
        this(context, null);
    }

    private WindowManagerImpl(Context context, Window parentWindow) {
        mContext = context;
        mParentWindow = parentWindow;
    }

    public WindowManagerImpl createLocalWindowManager(Window parentWindow) {
        return new WindowManagerImpl(mContext, parentWindow);
    }

    public WindowManagerImpl createPresentationWindowManager(Context displayContext) {
        return new WindowManagerImpl(displayContext, mParentWindow);
    }

    /** The token of a window without a parent window and a token of its own (InputMethodService's). */
    public void setDefaultToken(IBinder token) {
        mDefaultToken = token;
    }

    @Override
    public void addView(View view, ViewGroup.LayoutParams params) {
        applyTokens(params);
        mGlobal.addView(view, params, mContext.getDisplayNoVerify(), mParentWindow, mContext.getUserId());
    }

    @Override
    public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
        applyTokens(params);
        mGlobal.updateViewLayout(view, params);
    }

    private void applyTokens(ViewGroup.LayoutParams params) {
        if (!(params instanceof WindowManager.LayoutParams)) {
            throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        }
        final WindowManager.LayoutParams wparams = (WindowManager.LayoutParams) params;
        if (mDefaultToken != null && mParentWindow == null && wparams.token == null) {
            wparams.token = mDefaultToken;
        }
    }

    @Override
    public void removeView(View view) {
        mGlobal.removeView(view, false);
    }

    @Override
    public void removeViewImmediate(View view) {
        mGlobal.removeView(view, true);
    }

    @Override
    @Deprecated
    public Display getDefaultDisplay() {
        return mContext.getDisplayNoVerify();
    }

    /** No keyboard shortcuts yet (ADR-0041 a)'s input): the receiver hears of none. */
    @Override
    public void requestAppKeyboardShortcuts(KeyboardShortcutsReceiver receiver, int deviceId) {
        receiver.onKeyboardShortcutsReceived(new java.util.ArrayList<>());
    }

    /** The IME's touch region: none, as Minato shows no IME window. */
    @Override
    public Region getCurrentImeTouchRegion() {
        return null;
    }
}
