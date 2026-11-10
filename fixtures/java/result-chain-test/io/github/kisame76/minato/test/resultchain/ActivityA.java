package io.github.kisame76.minato.test.resultchain;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/** Starts B for a result and logs what comes back. */
public class ActivityA extends Activity {
    static final String TAG = "result-chain-test";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startActivityForResult(new Intent(this, ActivityB.class), 1);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.i(TAG, "A got " + requestCode + " " + resultCode);
        Log.i(TAG, "done");
    }
}
