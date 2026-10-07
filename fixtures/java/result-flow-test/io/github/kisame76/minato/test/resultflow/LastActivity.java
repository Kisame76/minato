package io.github.kisame76.minato.test.resultflow;

/**
 * Started by Main; its finishAndRemoveTask is a plain finish, as it is not the task's root, so Main comes back; "done"
 * closes the log at its destroy.
 */
public class LastActivity extends ResultActivity {
    @Override
    protected void onDestroy() {
        super.onDestroy();
        ResultApplication.log("done");
    }
}
