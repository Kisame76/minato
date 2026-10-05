// The written part of InputMethodManager (ADR-0043 step d), decision 2 as changed by the lead's OK of 2026-10-04),
// which tools/managergen joins with the API's methods: Android 15's client side on a device without an input method,
// until the keyboard step (ADR-0041 a)) connects the iPhone's keyboard. As AOSP's client (InputMethodManager.java at
// android-15.0.0_r1), it serves the focused view of the focused window: it makes the view's input connection when the
// view gains the focus (createInputConnection, 4999-5010; startInputInner, 3155-3330), finishes and closes it when
// another view is served, as RemoteInputConnectionImpl does, and answers from that (isActive, 1964-1985;
// isAcceptingText, 2005-2010; hideSoftInputFromWindow, 2465-2500). The input method service, which is not there, would
// say yes to a show or hide of the served view, as the AVD without one does; no input method binds, so the lists are
// empty and no connection is active. One manager per process, as AOSP's forContext keeps one per display (1595-1630),
// and Minato has one display.
import android.content.Context;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.UserHandle;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.WindowManager;

import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

class InputMethodManager {
    private static final InputMethodManager sInstance = new InputMethodManager();

    // AOSP's client state, guarded by the main thread, as its callers run there.
    private ViewRootImpl mCurRootView;
    private View mServedView;
    private View mNextServedView;
    private boolean mServedConnecting;
    private EditorInfo mCurrentEditorInfo;
    private ServedConnection mServedInputConnection;

    private InputMethodManager() {
    }

    /**
     * As AOSP's RemoteInputConnectionImpl, without an input method behind it: the served view's connection, called on
     * its thread, at once when the caller is on it (dispatch, 1405-1414).
     */
    private static final class ServedConnection {
        private final InputConnection mConnection;
        private final Handler mHandler;
        private final View mView;
        private final AtomicBoolean mDeactivateRequested = new AtomicBoolean(false);

        ServedConnection(InputConnection connection, Looper looper, View view) {
            mConnection = connection;
            mHandler = new Handler(looper);
            mView = view;
        }

        private void dispatch(Runnable runnable) {
            if (mHandler.getLooper().isCurrentThread()) {
                runnable.run();
                return;
            }
            mHandler.post(runnable);
        }

        /** As AOSP's (794-815): the composing text is finished, unless the connection is closed by then. */
        void finishComposingTextFromImm() {
            dispatch(() -> {
                if (!mDeactivateRequested.get()) {
                    mConnection.finishComposingText();
                }
            });
        }

        /** As AOSP's (356-402): the connection closes, then its view hears of it on the view's thread. */
        void deactivate() {
            if (mDeactivateRequested.getAndSet(true)) {
                return;
            }
            dispatch(() -> {
                mConnection.closeConnection();
                final Handler handler = mView.getHandler();
                if (handler != null) {
                    if (handler.getLooper().isCurrentThread()) {
                        mView.onInputConnectionClosedInternal();
                    } else {
                        handler.post(mView::onInputConnectionClosedInternal);
                    }
                }
            });
        }
    }

    /** @hide View's: the annotation of its handwriting delegation flags. */
    public @interface HandwritingDelegateFlags {
    }

    /** @hide SystemServiceRegistry's: the process's manager. */
    public static InputMethodManager forContext(Context context) {
        return sInstance;
    }

    /** @hide As AOSP's, for apps that still reach it: the process's manager. */
    public static InputMethodManager getInstance() {
        return sInstance;
    }

    /** @hide As AOSP's: the manager made so far, which is always the one. */
    public static InputMethodManager peekInstance() {
        return sInstance;
    }

    // ImeFocusController's, as AOSP's DelegateImpl (819-975).

    /** @hide */
    public void onPreWindowGainedFocus(ViewRootImpl viewRootImpl) {
        mCurRootView = viewRootImpl;
    }

    /** @hide */
    public void onPostWindowGainedFocus(View viewForWindowFocus, WindowManager.LayoutParams windowAttribute) {
        onViewFocusChangedInternal(viewForWindowFocus, true);
        // The served view without a connection is served again; with no input method, no connection is active.
        final boolean forceFocus = mServedView == viewForWindowFocus;
        if (mCurRootView != null && checkFocusInternal(forceFocus, mCurRootView)) {
            startInputOnWindowFocusGain();
        }
    }

    /** @hide */
    public void onWindowLostFocus(ViewRootImpl viewRootImpl) {
        // AOSP's keeps the window as the current root until another one gains the focus.
    }

    /** @hide */
    public void onViewFocusChanged(View view, boolean hasFocus) {
        onViewFocusChangedInternal(view, hasFocus);
    }

    /** @hide */
    public void onScheduledCheckFocus(ViewRootImpl viewRootImpl) {
        if (checkFocusInternal(false, viewRootImpl)) {
            startInputOnWindowFocusGain();
        }
    }

    /** @hide */
    public void onViewDetachedFromWindow(View view, ViewRootImpl viewRootImpl) {
        if (mCurRootView != view.getViewRootImpl()) {
            return;
        }
        if (mNextServedView == view) {
            mNextServedView = null;
        }
        if (mServedView == view) {
            viewRootImpl.dispatchCheckFocus();
        }
    }

    /** @hide */
    public void onWindowDismissed(ViewRootImpl viewRootImpl) {
        if (mCurRootView != viewRootImpl) {
            return;
        }
        if (mServedView != null) {
            finishInput();
        }
        mCurRootView = null;
    }

    /** As AOSP's (3559-3580): a focused view of the current window becomes the next served view. */
    private void onViewFocusChangedInternal(View view, boolean hasFocus) {
        if (view == null || view.isTemporarilyDetached()) {
            return;
        }
        final ViewRootImpl viewRootImpl = view.getViewRootImpl();
        if (mCurRootView != viewRootImpl || !view.hasImeFocus() || !view.hasWindowFocus()) {
            return;
        }
        // As AOSP's, a view that loses the focus stays served until another gains it, the view goes or the window.
        if (hasFocus) {
            mNextServedView = view;
        }
        viewRootImpl.dispatchCheckFocus();
    }

    /** @hide As AOSP's (3512-3525): serves the next served view now. */
    public void checkFocus() {
        if (mCurRootView != null && checkFocusInternal(false, mCurRootView)) {
            startInputOnWindowFocusGain();
        }
    }

    /** As AOSP's (3530-3555). */
    private boolean checkFocusInternal(boolean forceNewFocus, ViewRootImpl viewRootImpl) {
        if (mCurRootView != viewRootImpl || mServedView == mNextServedView && !forceNewFocus) {
            return false;
        }
        if (mNextServedView == null) {
            finishInput();
            return false;
        }
        mServedView = mNextServedView;
        if (mServedInputConnection != null) {
            mServedInputConnection.finishComposingTextFromImm();
        }
        return true;
    }

    private void startInputOnWindowFocusGain() {
        mCurrentEditorInfo = null;
        mServedConnecting = true;
        startInputInner();
    }

    /** As AOSP's (3155-3330) up to the service: the served view's input connection, made on its thread. */
    private boolean startInputInner() {
        final View view = getServedView();
        if (view == null) {
            return false;
        }
        final Handler handler = view.getHandler();
        if (handler == null) {
            return false;
        }
        if (handler.getLooper() != Looper.myLooper()) {
            handler.post(this::startInputOnWindowFocusGain);
            return false;
        }
        if (view.getWindowToken() == null) {
            return false;
        }
        // createInputConnection (4999-5010). The autofill ID stays null: autofill is off (step c2c3).
        final EditorInfo editorInfo = new EditorInfo();
        editorInfo.packageName = view.getContext().getOpPackageName();
        editorInfo.fieldId = view.getId();
        final InputConnection connection = view.onCreateInputConnection(editorInfo);
        if (connection == null) {
            editorInfo.fieldId = 0;
        }
        if (getServedView() != view || !mServedConnecting) {
            return false;
        }
        mCurrentEditorInfo = editorInfo;
        mServedConnecting = false;
        if (mServedInputConnection != null) {
            mServedInputConnection.deactivate();
            mServedInputConnection = null;
        }
        if (connection != null) {
            Handler connectionHandler = null;
            try {
                connectionHandler = connection.getHandler();
            } catch (AbstractMethodError ignored) {
                // As AOSP's: an app's InputConnection from before getHandler.
            }
            mServedInputConnection = new ServedConnection(connection,
                    connectionHandler != null ? connectionHandler.getLooper() : handler.getLooper(), view);
        }
        return true;
    }

    /** As AOSP's finishInputLocked (2106-2130) with clearConnectionLocked (2088-2098). */
    private void finishInput() {
        mNextServedView = null;
        if (mServedView != null) {
            mServedView = null;
            mServedConnecting = false;
            mCurrentEditorInfo = null;
            if (mServedInputConnection != null) {
                mServedInputConnection.deactivate();
                mServedInputConnection = null;
            }
        }
    }

    /** @hide BaseInputConnection's, as AOSP's (795-802): the served view's context, for content it receives. */
    Context getFallbackContextFromServedView() {
        if (mCurRootView == null) {
            return null;
        }
        return mServedView != null ? mServedView.getContext() : null;
    }

    /** @hide EditableInputConnection's: what an input method asks of the cursor's place, which none does. */
    public void setUpdateCursorAnchorInfoMode(int flags) {
    }

    private View getServedView() {
        return mCurRootView != null ? mServedView : null;
    }

    /** As AOSP's (1054-1058). */
    private boolean hasServedByInputMethod(View view) {
        final View servedView = getServedView();
        return servedView == view || servedView != null && servedView.checkInputConnectionProxy(view);
    }

    // The answers, as AOSP's client gives them; the service's part says yes.

    public boolean showSoftInput(View view, int flags) {
        return showSoftInput(view, flags, null);
    }

    public boolean showSoftInput(View view, int flags, android.os.ResultReceiver resultReceiver) {
        checkFocus();
        return hasServedByInputMethod(view);
    }

    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags) {
        return hideSoftInputFromWindow(windowToken, flags, null);
    }

    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags, android.os.ResultReceiver resultReceiver) {
        checkFocus();
        final View servedView = getServedView();
        return servedView != null && servedView.getWindowToken() == windowToken;
    }

    /** @hide TextView's and Editor's. */
    public boolean hideSoftInputFromView(View view, int flags) {
        checkFocus();
        return hasServedByInputMethod(view);
    }

    public boolean isActive() {
        checkFocus();
        return getServedView() != null && mCurrentEditorInfo != null;
    }

    public boolean isActive(View view) {
        checkFocus();
        return hasServedByInputMethod(view) && mCurrentEditorInfo != null;
    }

    /** @hide TextView's and BaseInputConnection's: no input method holds the connection. */
    public boolean hasActiveInputConnection(View view) {
        return false;
    }

    public boolean isAcceptingText() {
        checkFocus();
        return mServedInputConnection != null;
    }

    public boolean isFullscreenMode() {
        return false;
    }

    public boolean isInputMethodSuppressingSpellChecker() {
        return false;
    }

    public boolean isStylusHandwritingAvailable() {
        return false;
    }

    /** @hide View's and Editor's. */
    public boolean isStylusHandwritingAvailableAsUser(UserHandle user) {
        return false;
    }

    /** @hide Editor's: no input method asked for the cursor's place. */
    public boolean isCursorAnchorInfoEnabled() {
        return false;
    }

    public List<InputMethodInfo> getInputMethodList() {
        return Collections.emptyList();
    }

    public List<InputMethodInfo> getEnabledInputMethodList() {
        return Collections.emptyList();
    }

    public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(InputMethodInfo imi,
            boolean allowsImplicitlyEnabledSubtypes) {
        return Collections.emptyList();
    }

    public InputMethodInfo getCurrentInputMethodInfo() {
        return null;
    }

    public InputMethodSubtype getCurrentInputMethodSubtype() {
        return null;
    }

    /** As AOSP's (3052-3070): the served view's connection is made again. */
    public void restartInput(View view) {
        checkFocus();
        if (!hasServedByInputMethod(view)) {
            return;
        }
        mServedConnecting = true;
        startInputInner();
    }

    // What a text view tells the input method: no one listens.

    public void invalidateInput(View view) {
    }

    public void updateSelection(View view, int selStart, int selEnd, int candidatesStart, int candidatesEnd) {
    }

    public void updateCursorAnchorInfo(View view, CursorAnchorInfo cursorAnchorInfo) {
    }

    public void updateExtractedText(View view, int token, ExtractedText text) {
    }

    public void displayCompletions(View view, CompletionInfo[] completions) {
    }

    @Deprecated
    public void viewClicked(View view) {
    }

    public void startStylusHandwriting(View view) {
    }
}
