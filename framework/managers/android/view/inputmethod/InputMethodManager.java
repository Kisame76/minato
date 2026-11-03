// The written part of InputMethodManager (ADR-0043 step d), decision 2 as changed by the decision of 2026-10-04),
// which tools/managergen joins with the API's methods: Android 15's client side on a device without an input method,
// until the keyboard step (ADR-0041 a)) connects the iPhone's keyboard. As AOSP's client (InputMethodManager.java at
// android-15.0.0_r1), it serves the focused view of the focused window: it makes the view's input connection when the
// view gains the focus (createInputConnection, 4999-5010; startInputInner, 3155-3330), finishes and closes it when
// another view is served, as RemoteInputConnectionImpl does, and answers from that (isActive, 1964-1985;
// isAcceptingText, 2005-2010; hideSoftInputFromWindow, 2465-2500). The input method service, which is not there, would
// say yes to a show or hide of the served view, as the AVD without one does; no input method binds, so the lists are
// empty and no connection is active. One manager per process, as AOSP's forContext keeps one per display (1595-1630),
// and Minato has one display.
//
// The input method is the host's keyboard (ADR-0068 part 1): the manager tells the host with APKRT_REQUEST_IME which
// view is served (start, with the EditorInfo), when its keyboard is wanted (show), not wanted (hide) and when the view
// is served no more (stop), and the host answers with text records, which onHostText turns into calls on the served
// view's InputConnection, as an input method service's would be. Every connection has a serial, and a record for
// another serial is dropped, so that an edit meant for a view that has since lost the focus reaches no other. For the
// host's text bar and its tap outside the field the manager also tells where the served view is (field) and what text it
// holds (extract), which it asks of the view's connection as an input method that shows the text does, with
// getExtractedText(GET_EXTRACTED_TEXT_MONITOR) and then from the view's updateExtractedText and updateSelection.
import android.app.ActivityThread;
import android.content.Context;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.UserHandle;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.graphics.Rect;

import java.nio.charset.StandardCharsets;
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
    // The serial of the last connection the host was told of; 0 before the first. The host's records name one.
    private long mLastSerial;
    // Whether the keyboard is wanted: showSoftInput has been asked and no hide since.
    private boolean mKeyboardWanted;

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
        final long mSerial;
        /** The IMM's own BaseInputConnection(imm, false), which a view without a connection is served with. */
        boolean mFallback;

        // The extract the host's text bar shows (ExtractState), guarded by this.
        private EditorInfo mInfo;
        private boolean mMonitoring;
        private String mText = "";
        private int mTextStart;
        private int mSelStart;
        private int mSelEnd;
        private int mComposingStart = -1;
        private int mComposingEnd = -1;
        private String mLastExtract;
        private boolean mSendPending;
        // An engine's field (SDL2's DummyEdit) gives no extracted text: the text around the cursor instead, read again
        // after each of the host's edits, since such a view tells of no change.
        private boolean mSurrounding;
        // The field's rectangle that the host was last told.
        private final Rect mField = new Rect();
        private boolean mFieldSent;
        private ViewTreeObserver.OnGlobalLayoutListener mLayoutListener;
        private ViewTreeObserver.OnScrollChangedListener mScrollListener;

        ServedConnection(InputConnection connection, Looper looper, View view, long serial) {
            mConnection = connection;
            mSerial = serial;
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

        /**
         * A text record of the host (APKRT_TEXT_*): the call an input method makes, on the connection's thread. A
         * deleteSurroundingText of one character widens to a pair when the character is half of a surrogate pair, as
         * the input method does on its side (LatinIME's RichInputConnection), since the host has no copy of the text.
         */
        void hostText(int op, int arg0, int arg1, String text) {
            dispatch(() -> {
                if (mDeactivateRequested.get()) {
                    return;
                }
                switch (op) {
                    case TEXT_COMMIT:
                        mConnection.commitText(text, arg0);
                        break;
                    case TEXT_SET_COMPOSING:
                        mConnection.setComposingText(text, arg0);
                        break;
                    case TEXT_FINISH_COMPOSING:
                        mConnection.finishComposingText();
                        break;
                    case TEXT_DELETE_SURROUNDING:
                        mConnection.deleteSurroundingText(widened(arg0, true), widened(arg1, false));
                        break;
                    case TEXT_SET_SELECTION:
                        mConnection.setSelection(arg0, arg1);
                        break;
                    case TEXT_SET_COMPOSING_REGION:
                        mConnection.setComposingRegion(arg0, arg1);
                        break;
                    case TEXT_PERFORM_ACTION:
                        mConnection.performEditorAction(arg0);
                        break;
                    case TEXT_SEND_KEY:
                        sendKey(arg0);
                        break;
                    default:
                        break;
                }
                if (mSurrounding && readSurrounding()) {
                    sendExtract();
                }
            });
        }

        /** `length` code units before or after the cursor, two when the one is half of a surrogate pair. */
        private int widened(int length, boolean before) {
            if (length != 1) {
                return length;
            }
            final CharSequence around = before ? mConnection.getTextBeforeCursor(2, 0)
                    : mConnection.getTextAfterCursor(2, 0);
            if (around != null && around.length() == 2
                    && Character.isSurrogatePair(around.charAt(0), around.charAt(1))) {
                return 2;
            }
            return length;
        }

        /**
         * A key of the soft keyboard: its DOWN and UP, as LatinIME's sendDownUpKeyEvent makes them. Each is a call of
         * its own from the input method, so each is a message of its own on the connection's thread, and the DOWN
         * is handled before the UP is sent.
         */
        private void sendKey(int keyCode) {
            final long now = SystemClock.uptimeMillis();
            sendKeyEvent(now, KeyEvent.ACTION_DOWN, keyCode);
            mHandler.post(() -> {
                if (!mDeactivateRequested.get()) {
                    sendKeyEvent(now, KeyEvent.ACTION_UP, keyCode);
                }
            });
        }

        private void sendKeyEvent(long time, int action, int keyCode) {
            mConnection.sendKeyEvent(new KeyEvent(time, time, action, keyCode, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD,
                    0, KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE));
        }

        /**
         * The text bar's feed: the view's text through getExtractedText(GET_EXTRACTED_TEXT_MONITOR), as an input method
         * that shows it asks, and the view's place in the window. A view whose connection gives no extracted text, such
         * as an engine's (SDL2's SDLInputConnection), is read around the cursor instead, as input methods read it; a
         * view without a connection of its own gets neither.
         */
        void startFeeding(EditorInfo info) {
            if (mInfo != null || info == null) {
                return;
            }
            mInfo = info;
            sendField();
            final ViewTreeObserver observer = mView.getViewTreeObserver();
            if (observer.isAlive()) {
                mLayoutListener = this::sendField;
                mScrollListener = this::sendField;
                observer.addOnGlobalLayoutListener(mLayoutListener);
                observer.addOnScrollChangedListener(mScrollListener);
            }
            // On the connection's thread after the call that wanted the keyboard returns, as an input method starts.
            mHandler.post(() -> {
                if (mDeactivateRequested.get()) {
                    return;
                }
                final ExtractedTextRequest request = new ExtractedTextRequest();
                request.token = EXTRACT_TOKEN;
                request.hintMaxChars = EXTRACT_CHARS;
                final ExtractedText extracted = mConnection.getExtractedText(request,
                        InputConnection.GET_EXTRACTED_TEXT_MONITOR);
                if (extracted == null || extracted.text == null) {
                    if (readSurrounding()) {
                        synchronized (this) {
                            mSurrounding = true;
                            mMonitoring = true;
                        }
                        sendExtract();
                    }
                    return;
                }
                synchronized (this) {
                    mMonitoring = true;
                    mText = extracted.text.toString();
                    mTextStart = extracted.startOffset;
                    mSelStart = extracted.selectionStart + extracted.startOffset;
                    mSelEnd = extracted.selectionEnd + extracted.startOffset;
                    mComposingStart = mComposingEnd = -1;
                }
                sendExtract();
            });
        }

        /**
         * The text around the cursor from getTextBeforeCursor, getSelectedText and getTextAfterCursor, half the
         * extract's characters on each side, as the text from the field's start: such a connection does not say where
         * it begins.
         * False when the connection gives none of the three.
         */
        private boolean readSurrounding() {
            final CharSequence before = mConnection.getTextBeforeCursor(EXTRACT_CHARS / 2, 0);
            final CharSequence selected = mConnection.getSelectedText(0);
            final CharSequence after = mConnection.getTextAfterCursor(EXTRACT_CHARS / 2, 0);
            if (before == null && selected == null && after == null) {
                return false;
            }
            final String head = before == null ? "" : before.toString();
            final String middle = selected == null ? "" : selected.toString();
            synchronized (this) {
                mText = head + middle + (after == null ? "" : after);
                mTextStart = 0;
                mSelStart = head.length();
                mSelEnd = head.length() + middle.length();
                mComposingStart = mComposingEnd = -1;
            }
            return true;
        }

        /** The view's visible rectangle in the window's pixels, when the host has not had it as it is. */
        private void sendField() {
            final Rect visible = new Rect();
            if (mDeactivateRequested.get() || !mView.getGlobalVisibleRect(visible)) {
                return;
            }
            if (mFieldSent && visible.equals(mField)) {
                return;
            }
            mFieldSent = true;
            mField.set(visible);
            requestIme(mSerial, "field\t" + mSerial + "\t" + visible.left + "\t" + visible.top + "\t" + visible.right
                    + "\t" + visible.bottom);
        }

        /** The view's updateExtractedText: its text changed, wholly or in part, and where its selection is now. */
        void onUpdateExtractedText(int token, ExtractedText extracted) {
            if (token != EXTRACT_TOKEN || extracted == null || extracted.text == null) {
                return;
            }
            synchronized (this) {
                if (!mMonitoring) {
                    return;
                }
                if (extracted.partialStartOffset < 0) {
                    mText = extracted.text.toString();
                    mTextStart = extracted.startOffset;
                } else {
                    final int start = Math.max(0, Math.min(mText.length(), extracted.partialStartOffset - mTextStart));
                    final int end = Math.max(start, Math.min(mText.length(), extracted.partialEndOffset - mTextStart));
                    mText = mText.substring(0, start) + extracted.text + mText.substring(end);
                }
                mSelStart = extracted.selectionStart + extracted.startOffset;
                mSelEnd = extracted.selectionEnd + extracted.startOffset;
            }
            scheduleExtract();
        }

        /** The view's updateSelection: where the selection and the composing text are, in the whole text. */
        void onUpdateSelection(int selStart, int selEnd, int candidatesStart, int candidatesEnd) {
            synchronized (this) {
                if (!mMonitoring) {
                    return;
                }
                mSelStart = selStart;
                mSelEnd = selEnd;
                mComposingStart = candidatesStart;
                mComposingEnd = candidatesEnd;
            }
            scheduleExtract();
        }

        /** One extract for the changes of a message: the calls of an edit follow one another on the view's thread. */
        private void scheduleExtract() {
            synchronized (this) {
                if (mSendPending) {
                    return;
                }
                mSendPending = true;
            }
            mHandler.post(() -> {
                synchronized (this) {
                    mSendPending = false;
                }
                if (!mDeactivateRequested.get()) {
                    sendExtract();
                }
            });
        }

        /**
         * The host's "extract": where the text sent starts in the field's text, the selection and composing region in
         * the text sent, which is at most EXTRACT_CHARS characters around the selection, and the flags. A password's
         * text is only as many bullets as it has characters. The text is not logged.
         */
        private void sendExtract() {
            final String request;
            synchronized (this) {
                if (!mMonitoring) {
                    return;
                }
                final int length = mText.length();
                final int selStart = Math.max(0, Math.min(length, mSelStart - mTextStart));
                final int selEnd = Math.max(0, Math.min(length, mSelEnd - mTextStart));
                final int low = Math.min(selStart, selEnd);
                final int high = Math.max(selStart, selEnd);
                int from = 0;
                int to = length;
                if (length > EXTRACT_CHARS) {
                    from = Math.max(0, Math.min(length - EXTRACT_CHARS, (low + high) / 2 - EXTRACT_CHARS / 2));
                    to = from + EXTRACT_CHARS;
                    if (Character.isLowSurrogate(mText.charAt(from)) && from > 0) {
                        from--;
                    }
                    if (Character.isHighSurrogate(mText.charAt(to - 1))) {
                        to--;
                    }
                }
                final int type = mInfo.inputType;
                final int variation = type & EditorInfo.TYPE_MASK_VARIATION;
                final int typeClass = type & EditorInfo.TYPE_MASK_CLASS;
                final boolean password = typeClass == EditorInfo.TYPE_CLASS_TEXT
                        && (variation == EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
                                || variation == EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                                || variation == EditorInfo.TYPE_TEXT_VARIATION_WEB_PASSWORD)
                        || typeClass == EditorInfo.TYPE_CLASS_NUMBER
                                && variation == EditorInfo.TYPE_NUMBER_VARIATION_PASSWORD;
                final int flags = (type != EditorInfo.TYPE_NULL ? 1 : 0) | (password ? 2 : 0)
                        | ((type & EditorInfo.TYPE_TEXT_FLAG_MULTI_LINE) != 0 && typeClass == EditorInfo.TYPE_CLASS_TEXT
                                ? 4 : 0);
                final String shown;
                if (password) {
                    final StringBuilder bullets = new StringBuilder();
                    for (int i = from; i < to; i = mText.offsetByCodePoints(i, 1)) {
                        bullets.append('\u2022');
                    }
                    shown = bullets.toString();
                } else {
                    shown = mText.substring(from, to);
                }
                request = "extract\t" + mSerial + "\t" + (mTextStart + from) + "\t" + (selStart - from) + "\t"
                        + (selEnd - from) + "\t" + relative(mComposingStart - mTextStart, from, to) + "\t"
                        + relative(mComposingEnd - mTextStart, from, to) + "\t" + flags + "\t" + hintOf(mInfo) + "\t"
                        + shown;
                if (request.equals(mLastExtract)) {
                    return;
                }
                mLastExtract = request;
            }
            requestIme(mSerial, request);
        }

        /** A position of the text in the window of it that is sent, -1 for none or outside it. */
        private int relative(int position, int from, int to) {
            return position < from || position > to ? -1 : position - from;
        }

        /** The view's place is not followed, and its updates are not sent on (the view keeps its monitor; no call ends it). */
        private void stopFeeding() {
            final ViewTreeObserver observer = mView.getViewTreeObserver();
            if (observer.isAlive()) {
                if (mLayoutListener != null) {
                    observer.removeOnGlobalLayoutListener(mLayoutListener);
                }
                if (mScrollListener != null) {
                    observer.removeOnScrollChangedListener(mScrollListener);
                }
            }
            mLayoutListener = null;
            mScrollListener = null;
            synchronized (this) {
                mMonitoring = false;
            }
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

    // APKRT_TEXT_* (guest_interface.h).
    private static final int TEXT_COMMIT = 0;
    private static final int TEXT_SET_COMPOSING = 1;
    private static final int TEXT_FINISH_COMPOSING = 2;
    private static final int TEXT_DELETE_SURROUNDING = 3;
    private static final int TEXT_SET_SELECTION = 4;
    private static final int TEXT_SET_COMPOSING_REGION = 5;
    private static final int TEXT_PERFORM_ACTION = 6;
    private static final int TEXT_SEND_KEY = 7;
    // The longest request the runtime takes, less its NUL.
    private static final int REQUEST_BYTES = 511;
    // The text bar's extract: the token of the request, which the view's updates name, and the most characters sent.
    private static final int EXTRACT_TOKEN = 1;
    private static final int EXTRACT_CHARS = 4096;
    // The longest hint of an extract, in characters.
    private static final int EXTRACT_HINT_CHARS = 128;

    /**
     * @hide The input event receiver's: a text record of the host, for the connection with `serial`. It is dropped
     * unless that connection is the served one now.
     */
    public static void onHostText(int op, int arg0, int arg1, long serial, byte[] text) {
        final ServedConnection served = sInstance.mServedInputConnection;
        if (served == null || served.mSerial != serial) {
            return;
        }
        served.hostText(op, arg0, arg1, new String(text, StandardCharsets.UTF_8));
    }

    /** APKRT_REQUEST_IME: what the host should know of the served view; the host's answer is its text records. */
    private static void requestIme(long serial, String text) {
        ActivityThread.requestIme(serial, text);
    }

    /**
     * The "start" request of an editor: its numbers, its actionId and whether it has an actionLabel (whose Enter
     * performs actionId, as LatinIME's), and its hint, cut at a character so that the request fits.
     */
    private static String startRequest(EditorInfo info) {
        final StringBuilder request = new StringBuilder("start\t").append(info.inputType).append('\t')
                .append(info.imeOptions).append('\t').append(info.initialCapsMode).append('\t')
                .append(info.initialSelStart).append('\t').append(info.initialSelEnd).append('\t')
                .append(info.fieldId).append('\t').append(info.actionId).append('\t')
                .append(info.actionLabel != null ? 1 : 0).append('\t');
        int bytes = request.toString().getBytes(StandardCharsets.UTF_8).length;
        final CharSequence hint = info.hintText;
        for (int i = 0; hint != null && i < hint.length();) {
            final int point = Character.codePointAt(hint, i);
            final int size = new String(Character.toChars(point)).getBytes(StandardCharsets.UTF_8).length;
            if (point == 0 || bytes + size > REQUEST_BYTES) {
                break;
            }
            request.appendCodePoint(point);
            bytes += size;
            i += Character.charCount(point);
        }
        return request.toString();
    }

    /** An editor's hint for an extract: one line, which a tab or a new line would not let the host parse. */
    private static String hintOf(EditorInfo info) {
        final CharSequence hint = info.hintText;
        final StringBuilder line = new StringBuilder();
        for (int i = 0; hint != null && i < hint.length() && line.length() < EXTRACT_HINT_CHARS;) {
            final int point = Character.codePointAt(hint, i);
            line.appendCodePoint(point == 0 || point == '\t' || point == '\n' || point == '\r' ? ' ' : point);
            i += Character.charCount(point);
        }
        return line.toString();
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
        InputConnection connection = view.onCreateInputConnection(editorInfo);
        boolean fallback = false;
        if (connection == null) {
            // As AOSP's service-side mFallbackInputConnection = BaseInputConnection(imm, false) (InputMethodService
            // 4001-4010 at the tag): a view without a connection is served all the same, and the input method's
            // calls turn into key events for the served view (ADR-0069 step 3: a NativeActivity's decor view).
            editorInfo.fieldId = 0;
            connection = new BaseInputConnection(this, false);
            fallback = true;
        }
        if (getServedView() != view || !mServedConnecting) {
            return false;
        }
        mCurrentEditorInfo = editorInfo;
        mServedConnecting = false;
        endServedConnection();
        if (connection != null) {
            Handler connectionHandler = null;
            try {
                connectionHandler = connection.getHandler();
            } catch (AbstractMethodError ignored) {
                // As AOSP's: an app's InputConnection from before getHandler.
            }
            mServedInputConnection = new ServedConnection(connection,
                    connectionHandler != null ? connectionHandler.getLooper() : handler.getLooper(), view,
                    ++mLastSerial);
            mServedInputConnection.mFallback = fallback;
            requestIme(mServedInputConnection.mSerial, startRequest(editorInfo));
            // A keyboard that is up before this connection (a restart) feeds the host's text bar at once.
            if (!fallback && mKeyboardWanted) {
                mServedInputConnection.startFeeding(editorInfo);
            }
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
            endServedConnection();
        }
    }

    /** The served connection closes and the host hears that the view is served no more. */
    private void endServedConnection() {
        if (mServedInputConnection != null) {
            final long serial = mServedInputConnection.mSerial;
            mServedInputConnection.stopFeeding();
            mServedInputConnection.deactivate();
            mServedInputConnection = null;
            requestIme(serial, "stop");
        }
    }

    /** @hide BaseInputConnection's, as AOSP's (795-802): the served view's context, for content it receives. */
    Context getFallbackContextFromServedView() {
        if (mCurRootView == null) {
            return null;
        }
        return mServedView != null ? mServedView.getContext() : null;
    }

    /**
     * @hide BaseInputConnection's, as AOSP's (2335-2355): a key the input method sends goes to the target view's
     * window, or the served view's, as a key from the input method.
     */
    public void dispatchKeyEventFromInputMethod(View targetView, KeyEvent event) {
        ViewRootImpl viewRootImpl = targetView != null ? targetView.getViewRootImpl() : null;
        if (viewRootImpl == null) {
            final View servedView = getServedView();
            if (servedView != null) {
                viewRootImpl = servedView.getViewRootImpl();
            }
        }
        if (viewRootImpl != null) {
            viewRootImpl.dispatchKeyFromIme(event);
        }
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
        final boolean served = hasServedByInputMethod(view);
        if (served && mServedInputConnection != null) {
            mKeyboardWanted = true;
            requestIme(mServedInputConnection.mSerial, "show");
            // As a keyboard that shows the text asks when it starts: the host's text bar hears of the view then.
            if (!mServedInputConnection.mFallback) {
                mServedInputConnection.startFeeding(mCurrentEditorInfo);
            }
        }
        return served;
    }

    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags) {
        return hideSoftInputFromWindow(windowToken, flags, null);
    }

    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags, android.os.ResultReceiver resultReceiver) {
        checkFocus();
        final View servedView = getServedView();
        final boolean served = servedView != null && servedView.getWindowToken() == windowToken;
        if (served) {
            hideRequest();
        }
        return served;
    }

    /** @hide TextView's and Editor's. */
    public boolean hideSoftInputFromView(View view, int flags) {
        checkFocus();
        final boolean served = hasServedByInputMethod(view);
        if (served) {
            hideRequest();
        }
        return served;
    }

    /** The keyboard is not wanted: the host hears it with the live serial, or 0 without a connection. */
    private void hideRequest() {
        mKeyboardWanted = false;
        requestIme(mServedInputConnection != null ? mServedInputConnection.mSerial : 0, "hide");
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
        // As AOSP's: the fallback connection lives on the service's side, so the app has none served.
        return mServedInputConnection != null && !mServedInputConnection.mFallback;
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
        final ServedConnection served = mServedInputConnection;
        if (served != null && hasServedByInputMethod(view)) {
            served.onUpdateSelection(selStart, selEnd, candidatesStart, candidatesEnd);
        }
    }

    public void updateCursorAnchorInfo(View view, CursorAnchorInfo cursorAnchorInfo) {
    }

    public void updateExtractedText(View view, int token, ExtractedText text) {
        final ServedConnection served = mServedInputConnection;
        if (served != null && hasServedByInputMethod(view)) {
            served.onUpdateExtractedText(token, text);
        }
    }

    public void displayCompletions(View view, CompletionInfo[] completions) {
    }

    @Deprecated
    public void viewClicked(View view) {
    }

    public void startStylusHandwriting(View view) {
    }
}
