package android.inputmethodservice;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/** Compile only: Android's input method service, which framework.jar does not have (D8 leaves this class out). */
public class InputMethodService extends Service {
    public InputConnection getCurrentInputConnection() {
        throw new RuntimeException("Stub!");
    }

    public EditorInfo getCurrentInputEditorInfo() {
        throw new RuntimeException("Stub!");
    }

    public View onCreateInputView() {
        throw new RuntimeException("Stub!");
    }

    public void onStartInputView(EditorInfo info, boolean restarting) {
        throw new RuntimeException("Stub!");
    }

    @Override
    public IBinder onBind(Intent intent) {
        throw new RuntimeException("Stub!");
    }
}
