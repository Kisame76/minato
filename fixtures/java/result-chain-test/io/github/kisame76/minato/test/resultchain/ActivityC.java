package io.github.kisame76.minato.test.resultchain;

import android.app.Activity;
import android.os.Bundle;

/** Ends at once with a result. */
public class ActivityC extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setResult(2);
        finish();
    }
}
