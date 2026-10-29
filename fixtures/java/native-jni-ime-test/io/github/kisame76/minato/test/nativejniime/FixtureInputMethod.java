package io.github.kisame76.minato.test.nativejniime;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * ADR-0069 step 1's fixture input method, for the Android reference: when the NativeActivity's window is shown the
 * keyboard through JNI, it plays the calls a keyboard makes into a view without an editor, each a few milliseconds
 * after the last, and logs them and their answers under its own tag, which Minato does not have (its host posts the
 * same calls as text records). They go to the input method manager's fallback connection, which turns them into keys.
 */
public class FixtureInputMethod extends InputMethodService {
    static final String PACKAGE = "io.github.kisame76.minato.test.nativejniime";
    private static final String TAG = "native-jni-ime-test-reference";
    private static final long STEP_MS = 150;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mPlayed;

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        if (mPlayed || info == null || !PACKAGE.equals(info.packageName)) {
            return;
        }
        mPlayed = true;
        Log.i(TAG, "onStartInputView: inputType 0x" + Integer.toHexString(info.inputType) + ", imeOptions 0x"
                + Integer.toHexString(info.imeOptions) + ", selection " + info.initialSelStart + ","
                + info.initialSelEnd + ", fieldId " + info.fieldId + ", connection " + (connection() != null));
        if (connection() == null) {
            return;
        }
        Runnable[] calls = {
            () -> logged("commitText \"h\" 1", connection().commitText("h", 1)),
            () -> logged("commitText \"H\" 1", connection().commitText("H", 1)),
            () -> logged("commitText \"hi\" 1", connection().commitText("hi", 1)),
            () -> logged("commitText \"\u00e9\" 1", connection().commitText("\u00e9", 1)),
            () -> logged("deleteSurroundingText 1,0", connection().deleteSurroundingText(1, 0)),
            () -> logged("sendKeyEvent DEL down",
                    connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL))),
            () -> logged("sendKeyEvent DEL up",
                    connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL))),
            () -> logged("sendKeyEvent ENTER down",
                    connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))),
            () -> logged("sendKeyEvent ENTER up",
                    connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))),
            () -> logged("commitText \"\\n\" 1", connection().commitText("\n", 1)),
            () -> logged("performEditorAction 2", connection().performEditorAction(EditorInfo.IME_ACTION_GO)),
        };
        for (int i = 0; i < calls.length; i++) {
            mHandler.postDelayed(calls[i], STEP_MS * (i + 1));
        }
    }

    private InputConnection connection() {
        return getCurrentInputConnection();
    }

    private static void logged(String call, boolean result) {
        Log.i(TAG, "call " + call + " -> " + result);
    }
}
