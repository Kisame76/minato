package io.github.kisame76.minato.test.imefallback;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

/**
 * ADR-0069 step 3's test: a focused view that makes no input connection logs the key events an input method's calls
 * become (the fallback connection, BaseInputConnection(imm, false)), as native-jni-ime-test's NativeActivity logs
 * them. Minato's apk-run plays the keyboard; the log ends with "done" after the editor action's key.
 */
public class FallbackActivity extends Activity {
    private static final String TAG = "ime-fallback-test";

    private static class KeyView extends View {
        KeyView(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean dispatchKeyEvent(KeyEvent event) {
            String action = event.getAction() == KeyEvent.ACTION_DOWN ? "down"
                    : event.getAction() == KeyEvent.ACTION_UP ? "up" : "multiple";
            Log.i(TAG, "key " + action + " " + KeyEvent.keyCodeToString(event.getKeyCode()).substring(8)
                    + ", meta 0x" + Integer.toHexString(event.getMetaState()) + ", flags 0x"
                    + Integer.toHexString(event.getFlags()) + ", repeat " + event.getRepeatCount() + ", source 0x"
                    + Integer.toHexString(event.getSource()));
            if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER && (event.getFlags() & 0x16) == 0x16
                    && event.getAction() == KeyEvent.ACTION_UP) {
                Log.i(TAG, "isAcceptingText "
                        + getContext().getSystemService(InputMethodManager.class).isAcceptingText());
                Log.i(TAG, "done");
            }
            return true;
        }
    }

    private KeyView mView;
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new KeyView(this);
        setContentView(mView);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mView.requestFocus();
        mView.postDelayed(() -> {
            InputMethodManager imm = getSystemService(InputMethodManager.class);
            boolean active = imm.isActive(mView);
            boolean shown = imm.showSoftInput(mView, InputMethodManager.SHOW_FORCED);
            Log.i(TAG + "-state", "isActive " + active + ", showSoftInput " + shown);
        }, 1500);
    }
}
