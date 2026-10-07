package io.github.kisame76.minato.test.activityflow;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/**
 * Logs an activity's lifecycle under its name, its class's without "Activity", and reports its stops, its destroy and
 * its focus to the flow. Its focus is counted, not logged: when it comes among the lifecycle's callbacks depends on
 * timing on Android.
 */
public abstract class FlowActivity extends Activity {
    String name() {
        String name = getClass().getSimpleName();
        return name.substring(0, name.length() - "Activity".length());
    }

    /** The intent's flags, for the activities that the app starts; the launcher's come from whoever started it. */
    String intentFlags() {
        return ", flags 0x" + Integer.toHexString(getIntent().getFlags());
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FlowApplication.log(name() + " onCreate, saved state " + (savedInstanceState != null) + intentFlags());
        TextView text = new TextView(this);
        text.setText(name());
        setContentView(text);
        FlowApplication.get().created(this);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        FlowApplication.log(name() + " onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        FlowApplication.log(name() + " onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        FlowApplication.log(name() + " onResume");
    }

    @Override
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {
        super.onTopResumedActivityChanged(isTopResumedActivity);
        FlowApplication.log(name() + " onTopResumedActivityChanged " + isTopResumedActivity);
    }

    /** Android's hint that the user leaves the activity, as at a start from it, but not at its finish. */
    @Override
    protected void onUserLeaveHint() {
        super.onUserLeaveHint();
        FlowApplication.log(name() + " onUserLeaveHint");
    }

    @Override
    protected void onPause() {
        super.onPause();
        FlowApplication.log(name() + " onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        FlowApplication.log(name() + " onStop");
        FlowApplication.get().count(this, FlowApplication.STOPS);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        FlowApplication.log(name() + " onSaveInstanceState");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        FlowApplication.log(name() + " onDestroy, finishing " + isFinishing());
        FlowApplication.get().count(this, FlowApplication.DESTROYS);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        FlowApplication.get().count(this, hasFocus ? FlowApplication.GAINS : FlowApplication.LOSSES);
    }
}
