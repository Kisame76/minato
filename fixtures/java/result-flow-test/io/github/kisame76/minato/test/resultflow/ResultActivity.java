package io.github.kisame76.minato.test.resultflow;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;

/**
 * Logs an activity's lifecycle and its results under its name, its class's without "Activity", with the activity that
 * waits for its result, and reports its resumes, pauses, stops, destroy and focus to the flow. Its focus is counted,
 * not logged: when it comes among the lifecycle's callbacks depends on timing on Android.
 */
public abstract class ResultActivity extends Activity {
    String name() {
        String name = getClass().getSimpleName();
        return name.substring(0, name.length() - "Activity".length());
    }

    /** The intent's flags, for the activities that the app starts; the launcher's come from whoever started it. */
    String intentFlags() {
        return ", flags 0x" + Integer.toHexString(getIntent().getFlags());
    }

    /** The activity that started this one for a result and gets it, as Android names it, or "none". */
    private String caller() {
        ComponentName caller = getCallingActivity();
        return caller == null ? "none" : caller.getShortClassName() + " of " + getCallingPackage();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ResultApplication.log(name() + " onCreate, saved state " + (savedInstanceState != null) + intentFlags()
                + ", calling activity " + caller());
        TextView text = new TextView(this);
        text.setText(name());
        setContentView(text);
        ResultApplication.get().created(this);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        ResultApplication.log(name() + " onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        ResultApplication.log(name() + " onStart");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        ResultApplication.log(name() + " onActivityResult " + requestCode + ", result " + resultCode + ", from "
                + (data == null ? "none" : data.getStringExtra("from")));
    }

    @Override
    protected void onResume() {
        super.onResume();
        ResultApplication.log(name() + " onResume");
        ResultApplication.get().count(this, ResultApplication.RESUMES);
    }

    @Override
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {
        super.onTopResumedActivityChanged(isTopResumedActivity);
        ResultApplication.log(name() + " onTopResumedActivityChanged " + isTopResumedActivity);
    }

    @Override
    protected void onUserLeaveHint() {
        super.onUserLeaveHint();
        ResultApplication.log(name() + " onUserLeaveHint");
    }

    @Override
    protected void onPause() {
        super.onPause();
        ResultApplication.log(name() + " onPause");
        ResultApplication.get().count(this, ResultApplication.PAUSES);
    }

    @Override
    protected void onStop() {
        super.onStop();
        ResultApplication.log(name() + " onStop");
        ResultApplication.get().count(this, ResultApplication.STOPS);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        ResultApplication.log(name() + " onSaveInstanceState");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ResultApplication.log(name() + " onDestroy, finishing " + isFinishing());
        ResultApplication.get().count(this, ResultApplication.DESTROYS);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            ResultApplication.get().count(this, ResultApplication.GAINS);
        }
    }
}
