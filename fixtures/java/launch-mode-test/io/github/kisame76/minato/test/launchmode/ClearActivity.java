package io.github.kisame76.minato.test.launchmode;

/** Started with NEW_TASK and CLEAR_TASK, the task's root then; "done" closes the log at its destroy. */
public class ClearActivity extends ModeActivity {
    @Override
    protected void onDestroy() {
        super.onDestroy();
        ModeApplication.log("done");
    }
}
