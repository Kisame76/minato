package io.github.kisame76.minato.test.imesurrounding;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.inputmethod.EditorInfo;

/**
 * ime-surrounding-test-return's fixture input method, for the Android reference only: when the twin's field is served,
 * it types "hi" and presses Enter, which for the field's action (IME_ACTION_UNSPECIFIED) is performEditorAction, as
 * LatinIME's Enter; it logs when its window goes. Minato has no input method service: the host's keyboard makes the
 * same calls (SoftKeyboard.returnKey).
 */
public class FixtureInputMethod extends InputMethodService {
    private static final long STEP_MS = 500;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mPlayed;

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        if (mPlayed || info == null || !SurroundingActivity.RETURN_PACKAGE.equals(info.packageName)) {
            return;
        }
        mPlayed = true;
        mHandler.postDelayed(() -> getCurrentInputConnection().commitText("hi", 1), STEP_MS);
        mHandler.postDelayed(() -> getCurrentInputConnection().performEditorAction(EditorInfo.IME_ACTION_UNSPECIFIED),
                STEP_MS * 2);
    }

    @Override
    public void onWindowHidden() {
        if (mPlayed) {
            Log.i(SurroundingActivity.RETURN_TAG, "input method: window hidden");
        }
    }
}
