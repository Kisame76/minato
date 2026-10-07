package io.github.kisame76.minato.test.key;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.method.DigitsKeyListener;
import android.util.Log;
import android.view.KeyEvent;
import android.widget.EditText;

/**
 * ADR-0045 step 7c1's test of the window's key stages: an EditText with a DigitsKeyListener, focused, into whose window
 * the activity injects KEYCODE_1, KEYCODE_A and KEYCODE_DEL, each down and up, once the window has focus, as an app
 * injects keys (Window.injectInputEvent). The lines name what reaches the EditText, its text after each key, which
 * refuses the letter, and the touch mode, which a typing key leaves; then "done".
 */
public class KeyActivity extends Activity {
    private static final String TAG = "key-test";
    private static final int[] KEYS = {KeyEvent.KEYCODE_1, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_DEL};

    private EditText mEdit;
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mEdit = new EditText(this);
        mEdit.setKeyListener(DigitsKeyListener.getInstance("0123456789"));
        mEdit.setOnKeyListener((view, keyCode, event) -> {
            Log.i(TAG, "edit: " + (event.getAction() == KeyEvent.ACTION_DOWN ? "down " : "up ")
                    + KeyEvent.keyCodeToString(keyCode) + ", unicode 0x" + Integer.toHexString(event.getUnicodeChar())
                    + ", device " + event.getDeviceId());
            return false;
        });
        setContentView(mEdit);
        mEdit.requestFocus();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            Log.i(TAG, "edit: focused " + mEdit.isFocused() + ", input type 0x" + Integer.toHexString(mEdit.getInputType())
                    + ", key listener " + mEdit.getKeyListener().getClass().getName());
            type(0);
        }
    }

    /** Injects the key at `index`, down and up, and logs the text once the window has handled both. */
    private void type(int index) {
        if (index == KEYS.length) {
            Log.i(TAG, "done");
            return;
        }
        long now = SystemClock.uptimeMillis();
        getWindow().injectInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KEYS[index], 0));
        getWindow().injectInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KEYS[index], 0));
        mEdit.post(() -> {
            Log.i(TAG, KeyEvent.keyCodeToString(KEYS[index]) + ": text \"" + mEdit.getText() + "\", in touch mode "
                    + mEdit.isInTouchMode());
            type(index + 1);
        });
    }
}
