package io.github.kisame76.minato.test.launchmode;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;

/**
 * Logs an activity's lifecycle, its new intents and its results under its name, its class's without "Activity", and
 * reports its creates, new intents, resumes, pauses, stops, destroy and focus to the flow. Its focus is counted, not
 * logged: when it comes among the lifecycle's callbacks depends on timing on Android.
 */
public abstract class ModeActivity extends Activity {
    /** Whether it is between its onResume and its onPause. */
    boolean mResumedNow;

    String name() {
        String name = getClass().getSimpleName();
        return name.substring(0, name.length() - "Activity".length());
    }

    /** An intent's flags and its "n" extra; the launcher's flags come from whoever started it, so they are left out. */
    static String describe(Intent intent) {
        String flags = Intent.ACTION_MAIN.equals(intent.getAction()) ? ""
                : ", flags 0x" + Integer.toHexString(intent.getFlags());
        String n = intent.getStringExtra(ModeApplication.N);
        return flags + (n != null ? ", n " + n : "");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ModeApplication.log(name() + " onCreate, saved state " + (savedInstanceState != null) + describe(getIntent()));
        TextView text = new TextView(this);
        text.setText(name());
        setContentView(text);
        ModeApplication.get().created(this);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        ModeApplication.log(name() + " onNewIntent" + describe(intent));
        ModeApplication.get().count(this, ModeApplication.NEW_INTENTS);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        ModeApplication.log(name() + " onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        ModeApplication.log(name() + " onStart");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        ModeApplication.log(name() + " onActivityResult " + requestCode + ", result " + resultCode);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mResumedNow = true;
        ModeApplication.log(name() + " onResume");
        ModeApplication.get().count(this, ModeApplication.RESUMES);
    }

    @Override
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {
        super.onTopResumedActivityChanged(isTopResumedActivity);
        ModeApplication.log(name() + " onTopResumedActivityChanged " + isTopResumedActivity);
    }

    @Override
    protected void onUserLeaveHint() {
        super.onUserLeaveHint();
        ModeApplication.log(name() + " onUserLeaveHint");
    }

    @Override
    protected void onPause() {
        super.onPause();
        mResumedNow = false;
        ModeApplication.log(name() + " onPause");
        ModeApplication.get().count(this, ModeApplication.PAUSES);
    }

    @Override
    protected void onStop() {
        super.onStop();
        ModeApplication.log(name() + " onStop");
        ModeApplication.get().count(this, ModeApplication.STOPS);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        ModeApplication.log(name() + " onSaveInstanceState");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ModeApplication.log(name() + " onDestroy, finishing " + isFinishing());
        ModeApplication.get().count(this, ModeApplication.DESTROYS);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            ModeApplication.get().count(this, ModeApplication.GAINS);
        }
    }
}
