package io.github.kisame76.minato.test.enter;

import android.content.pm.PackageManager;
import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * enter-test's fixture input method, for the Android reference: for each of EnterActivity's fields it types the field's
 * keys a few milliseconds apart, making the calls LatinIME (android15-release) makes for them, which Minato's host
 * makes from UIKit's keys (SoftKeyboard): Enter performs the editor's action, actionId for an action label, and is a
 * new line for IME_ACTION_NONE and IME_FLAG_NO_ENTER_ACTION, the ENTER key before Jelly Bean (InputLogic's
 * handleNonFunctionalEvent, InputTypeUtils.getImeOptionsActionIdFromEditorInfo, sendKeyCodePoint); the delete is the
 * DEL key before Jelly Bean and without an editor, else deleteSurroundingText (handleBackspaceEvent); a digit is its
 * key outside the text class (sendKeyCodePoint); the rest are commits. Minato has no input method service.
 */
public class FixtureInputMethod extends InputMethodService {
    private static final long STEP_MS = 150;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final boolean[] mPlayed = new boolean[EnterActivity.FIELDS.length];

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        if (info == null || !EnterActivity.PACKAGE.equals(info.packageName)) {
            return;
        }
        // Each field once, known by its view's ID, its place in the list plus 1; a restart plays nothing.
        final int field = info.fieldId - 1;
        if (field < 0 || field >= EnterActivity.FIELDS.length || mPlayed[field]) {
            return;
        }
        mPlayed[field] = true;
        final String keys = EnterActivity.KEYS[field];
        final boolean beforeJellyBean = targetSdk(info.packageName) < 16;
        for (int i = 0; i < keys.length(); i++) {
            final char key = keys.charAt(i);
            mHandler.postDelayed(() -> type(key, info, beforeJellyBean), STEP_MS * (i + 1));
        }
    }

    private void type(char key, EditorInfo info, boolean beforeJellyBean) {
        InputConnection connection = getCurrentInputConnection();
        if (connection == null) {
            return;
        }
        if (key == '\n') {
            int action = enterAction(info);
            if (action != EditorInfo.IME_ACTION_NONE) {
                connection.performEditorAction(action);
            } else if (beforeJellyBean) {
                sendDownUp(connection, KeyEvent.KEYCODE_ENTER);
            } else {
                connection.commitText("\n", 1);
            }
        } else if (key == '\b') {
            if (beforeJellyBean || info.inputType == InputType.TYPE_NULL) {
                sendDownUp(connection, KeyEvent.KEYCODE_DEL);
            } else {
                connection.deleteSurroundingText(1, 0);
            }
        } else if (key >= '0' && key <= '9'
                && (info.inputType & InputType.TYPE_MASK_CLASS) != InputType.TYPE_CLASS_TEXT) {
            sendDownUp(connection, KeyEvent.KEYCODE_0 + key - '0');
        } else {
            connection.commitText(String.valueOf(key), 1);
        }
    }

    /** InputTypeUtils.getImeOptionsActionIdFromEditorInfo, with the custom label's action as its actionId. */
    private static int enterAction(EditorInfo info) {
        if ((info.imeOptions & EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0) {
            return EditorInfo.IME_ACTION_NONE;
        }
        if (info.actionLabel != null) {
            return info.actionId;
        }
        return info.imeOptions & EditorInfo.IME_MASK_ACTION;
    }

    /** LatinIME's sendDownUpKeyEvent. */
    private static void sendDownUp(InputConnection connection, int code) {
        long time = SystemClock.uptimeMillis();
        int flags = KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE;
        connection.sendKeyEvent(new KeyEvent(time, time, KeyEvent.ACTION_DOWN, code, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags));
        connection.sendKeyEvent(new KeyEvent(time, time, KeyEvent.ACTION_UP, code, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags));
    }

    private int targetSdk(String packageName) {
        try {
            return getPackageManager().getApplicationInfo(packageName, 0).targetSdkVersion;
        } catch (PackageManager.NameNotFoundException e) {
            return 35;
        }
    }
}
