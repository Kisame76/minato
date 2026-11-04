package io.github.kisame76.minato.test.menupause;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/**
 * An activity that logs its lifecycle callbacks and the focus, in the order they come, under one tag with its own
 * name in front of each line.
 */
abstract class LoggedActivity extends Activity {
    static final String TAG = "menu-pause-test";

    private void say(String text) {
        Log.i(TAG, getClass().getSimpleName() + " " + text);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        say("onCreate");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        say("onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        say("onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        say("onWindowFocusChanged " + hasFocus);
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("onPause");
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        say("onSaveInstanceState");
    }

    @Override
    protected void onStop() {
        super.onStop();
        say("onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("onDestroy");
    }
}
