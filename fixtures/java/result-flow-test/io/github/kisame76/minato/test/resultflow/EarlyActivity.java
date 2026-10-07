package io.github.kisame76.minato.test.resultflow;

import android.os.Bundle;

/** Started by Main for result 2; finishes in its onCreate, so that Main gets RESULT_CANCELED without having stopped. */
public class EarlyActivity extends ResultActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ResultApplication.log("Early finishes in onCreate");
        finish();
    }
}
