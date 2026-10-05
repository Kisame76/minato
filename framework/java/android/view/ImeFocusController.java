/*
 * Our ImeFocusController, under AOSP's name (ADR-0039 step c2c3; ADR-0043 step d)): what a window's ViewRootImpl and
 * its views tell the input method manager about focus, as AOSP's does (ImeFocusController.java at android-15.0.0_r1).
 * Our InputMethodManager keeps the served view from it as Android's client does, without an input method behind it,
 * so a focused editor is served and nothing shows a keyboard (ADR-0041 decision 2). AOSP's hands an input event to an
 * input method first; Minato's windows have no input yet (ADR-0041 a)).
 */
package android.view;

import android.content.Context;
import android.view.inputmethod.InputMethodManager;

/** @hide */
public final class ImeFocusController {
    private final ViewRootImpl mViewRootImpl;
    private final Context mContext;
    private boolean mHasImeFocus;

    ImeFocusController(ViewRootImpl viewRootImpl, Context context) {
        mViewRootImpl = viewRootImpl;
        mContext = context;
    }

    private InputMethodManager getImm() {
        return mContext.getSystemService(InputMethodManager.class);
    }

    /** A traversal: the window's flags may now allow or forbid an input method. */
    void onTraversal(boolean hasWindowFocus, WindowManager.LayoutParams windowAttribute) {
        final boolean hasImeFocus = WindowManager.LayoutParams.mayUseInputMethod(windowAttribute.flags);
        if (!hasWindowFocus || isInLocalFocusMode(windowAttribute) || hasImeFocus == mHasImeFocus) {
            return;
        }
        mHasImeFocus = hasImeFocus;
        if (mHasImeFocus) {
            getImm().onPreWindowGainedFocus(mViewRootImpl);
            final View focusedView = mViewRootImpl.getView().findFocus();
            getImm().onPostWindowGainedFocus(focusedView != null ? focusedView : mViewRootImpl.getView(),
                    windowAttribute);
        }
    }

    /** Before the window's views hear of its focus. */
    void onPreWindowFocus(boolean hasWindowFocus, WindowManager.LayoutParams windowAttribute) {
        mHasImeFocus = WindowManager.LayoutParams.mayUseInputMethod(windowAttribute.flags);
        if (!hasWindowFocus || !mHasImeFocus || isInLocalFocusMode(windowAttribute)) {
            if (!hasWindowFocus) {
                getImm().onWindowLostFocus(mViewRootImpl);
            }
        } else {
            getImm().onPreWindowGainedFocus(mViewRootImpl);
        }
    }

    /** After the window's views heard of its focus: the focused view, or the window's root, is served. */
    void onPostWindowFocus(View focusedView, boolean hasWindowFocus, WindowManager.LayoutParams windowAttribute) {
        if (!hasWindowFocus || !mHasImeFocus || isInLocalFocusMode(windowAttribute)) {
            return;
        }
        getImm().onPostWindowGainedFocus(focusedView != null ? focusedView : mViewRootImpl.getView(), windowAttribute);
    }

    void onScheduledCheckFocus() {
        getImm().onScheduledCheckFocus(mViewRootImpl);
    }

    void onViewFocusChanged(View view, boolean hasFocus) {
        getImm().onViewFocusChanged(view, hasFocus);
    }

    void onViewDetachedFromWindow(View view) {
        getImm().onViewDetachedFromWindow(view, mViewRootImpl);
    }

    void onWindowDismissed() {
        getImm().onWindowDismissed(mViewRootImpl);
        mHasImeFocus = false;
    }

    private static boolean isInLocalFocusMode(WindowManager.LayoutParams windowAttribute) {
        return (windowAttribute.flags & WindowManager.LayoutParams.FLAG_LOCAL_FOCUS_MODE) != 0;
    }

    boolean hasImeFocus() {
        return mHasImeFocus;
    }
}
