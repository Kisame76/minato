package io.github.kisame76.minato.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/** The launcher: throws from a message on the main thread, which no code of the app catches. */
public class CrashActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(CrashApplication.TAG, "onCreate");
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                Log.i(CrashApplication.TAG, "throws on the main thread");
                throw new IllegalStateException("thrown on the main thread");
            }
        });
    }
}
