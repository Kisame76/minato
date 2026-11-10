package io.github.kisame76.minato.test.resultchain;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/** Starts C for a result and, when C's comes, finishes with a result of its own from inside onActivityResult. */
public class ActivityB extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startActivityForResult(new Intent(this, ActivityC.class), 2);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.i(ActivityA.TAG, "B got " + requestCode + " " + resultCode);
        setResult(3);
        finish();
    }
}
