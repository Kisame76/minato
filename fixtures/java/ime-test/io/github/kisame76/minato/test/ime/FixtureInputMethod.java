package io.github.kisame76.minato.test.ime;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * ADR-0068 step 2a's fixture input method, for the Android reference: when ImeActivity's field is served, it plays the
 * calls a keyboard makes while someone types "hello", corrects the last letter and presses the action key, each a
 * few milliseconds after the last. Minato has no input method service; apk-run's text steps post the same calls.
 */
public class FixtureInputMethod extends InputMethodService {
    private static final long STEP_MS = 150;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mPlayed;

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        if (mPlayed || info == null || !ImeActivity.PACKAGE.equals(info.packageName)) {
            return;
        }
        mPlayed = true;
        Runnable[] calls = {
            () -> connection().setComposingText("h", 1),
            () -> connection().setComposingText("he", 1),
            () -> connection().commitText("hello", 1),
            () -> connection().deleteSurroundingText(1, 0),
            () -> connection().performEditorAction(EditorInfo.IME_ACTION_GO),
            () -> connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL)),
            () -> connection().sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL)),
        };
        for (int i = 0; i < calls.length; i++) {
            mHandler.postDelayed(calls[i], STEP_MS * (i + 1));
        }
    }

    private InputConnection connection() {
        return getCurrentInputConnection();
    }
}
