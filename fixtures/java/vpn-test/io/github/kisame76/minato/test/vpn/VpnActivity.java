package io.github.kisame76.minato.test.vpn;

import android.app.Activity;
import android.content.Intent;
import android.net.VpnService;
import android.os.Bundle;
import android.util.Log;

/**
 * The launcher activity of vpn-test: once the application's rounds are over, the app does what a VPN app does with the
 * intent that prepare() returned and starts it for a result. The user says no (BACK, on the phone), so the dialog ends
 * with RESULT_CANCELED, and this activity only pauses while it is up, as the dialog does not cover it.
 */
public class VpnActivity extends Activity {
    private static final String TAG = "vpn-test";
    private static final int REQUEST = 7;
    /** Set by the result: the resume that follows it ends the test, so that the last lines are in a fixed order. */
    private boolean mResulted;
    /** Set once the consent intent is started: the lifecycle lines before it race with the application's rounds. */
    private boolean mStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        new Thread(() -> {
            VpnApplication.awaitRounds();
            runOnUiThread(this::startConsent);
        }).start();
    }

    private void startConsent() {
        Intent intent = VpnService.prepare(this);
        if (intent == null) {
            Log.i(TAG, "consent: not needed");
            VpnApplication.finish();
            return;
        }
        try {
            startActivityForResult(intent, REQUEST);
            mStarted = true;
            Log.i(TAG, "consent: started");
        } catch (Throwable t) {
            Log.i(TAG, "consent: start throws " + t.getClass().getName());
            VpnApplication.finish();
        }
    }

    @Override
    protected void onPause() {
        if (mStarted) Log.i(TAG, "activity: onPause");
        super.onPause();
    }

    @Override
    protected void onStop() {
        if (mStarted) Log.i(TAG, "activity: onStop");
        super.onStop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mResulted) {
            Log.i(TAG, "activity: onResume");
            VpnApplication.finish();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.i(TAG, "consent: result request " + requestCode + ", code " + resultCode + ", data "
            + (data == null ? "null" : "not null"));
        super.onActivityResult(requestCode, resultCode, data);
        mResulted = true;
    }
}
