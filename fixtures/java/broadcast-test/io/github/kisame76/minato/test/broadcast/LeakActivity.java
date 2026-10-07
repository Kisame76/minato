package io.github.kisame76.minato.test.broadcast;

import android.app.Activity;
import android.content.IntentFilter;
import android.os.Bundle;
import android.util.Log;

/**
 * Registers a receiver for LEAK and finishes without unregistering it, as an app that forgets to: Android logs the leak
 * at the destroy and unregisters the receiver. Its onDestroy tells the test, which then sends LEAK.
 */
public class LeakActivity extends Activity {
    /** Whether the leaked receiver got a broadcast. */
    static volatile boolean sReceived;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        registerReceiver(new BroadcastActivity.Receiver("leaked", (receiver, intent) -> sReceived = true),
                new IntentFilter(BroadcastActivity.LEAK), RECEIVER_NOT_EXPORTED);
        Log.i(BroadcastActivity.TAG, "leak: Leak registered its receiver and finishes");
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(BroadcastActivity.TAG, "leak: Leak destroyed");
        BroadcastActivity.sTest.leakDestroyed();
    }
}
