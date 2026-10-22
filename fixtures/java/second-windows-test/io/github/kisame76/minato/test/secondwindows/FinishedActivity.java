package io.github.kisame76.minato.test.secondwindows;

import android.app.Activity;
import android.app.Dialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/** An activity that finishes at once and shows a dialog once it is destroyed: the dialog meets a dead token. */
public class FinishedActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            SecondWindowsActivity.log("finished activity: " + SecondWindowsActivity.outcome(() -> {
                Dialog dialog = new Dialog(this);
                dialog.show();
                boolean showing = dialog.isShowing();
                dialog.dismiss();
                return "showing " + showing;
            }));
            SecondWindowsActivity.sInstance.translucentPart();
        }, 600);
    }
}
