package io.github.kisame76.minato.test.back;

import android.content.Intent;
import android.os.Bundle;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

/**
 * The launcher activity: it starts Second at its first resume. back-test logs "done" at its onBackPressed, before
 * Android moves the task to the back; back-test-callback registers a plain callback, which logs "done" when it is
 * invoked and goes, so that the next BACK reaches the activity's default callback.
 */
public class RootActivity extends BackActivity {
    private boolean mStarted;
    private final OnBackInvokedCallback mCallback = new OnBackInvokedCallback() {
        @Override
        public void onBackInvoked() {
            log("callback onBackInvoked");
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(this);
            done();
        }
    };

    @Override
    String name() {
        return "root";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (callbacks()) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    mCallback);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            log("starts second");
            startActivity(new Intent(this, SecondActivity.class));
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        log("onBackPressed");
        done();
        super.onBackPressed();
    }
}
