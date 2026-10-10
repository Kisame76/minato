package io.github.kisame76.minato.test.alert;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;

import java.util.Arrays;

/**
 * android.app.AlertDialog as an app uses it (the Unity test APK probe: UnityPlayer.<init> resolves the class): the builder, the
 * title, message and buttons, items, single and multiple choice, a custom view, the clicks and the listeners. The
 * dialog has no window of its own on Minato yet (gap 4), so the test reads its views and clicks the buttons in code;
 * how a dialog looks is for the window tests. It runs on the main thread when the launcher activity first has the
 * window focus.
 */
public class AlertActivity extends Activity {
    static final String TAG = "alert-test";
    private static final int SETTLE_MS = 600;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private static DialogInterface.OnClickListener clicks(String name) {
        return (d, which) -> log(name + " onClick which=" + which);
    }

    private static String button(AlertDialog d, int which) {
        Button b = d.getButton(which);
        return b == null ? "none" : "\"" + b.getText() + "\" visibility " + b.getVisibility();
    }

    private View byName(AlertDialog d, String name) {
        return d.findViewById(getResources().getIdentifier(name, "id", "android"));
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.post(this::buttons);
        }
    }

    private void settle(Runnable next) {
        mHandler.postDelayed(next, SETTLE_MS);
    }

    private void buttons() {
        AlertDialog d = new AlertDialog.Builder(this)
                .setTitle("Title").setMessage("Message")
                .setPositiveButton("Yes", clicks("yes"))
                .setNegativeButton("No", clicks("no"))
                .setNeutralButton("Maybe", clicks("maybe"))
                .setOnDismissListener(x -> log("buttons dismiss-listener"))
                .setOnCancelListener(x -> log("buttons cancel-listener"))
                .create();
        log("created: showing " + d.isShowing() + ", is a Dialog " + (d instanceof android.app.Dialog));
        d.show();
        log("showing " + d.isShowing());
        TextView message = (TextView) d.findViewById(android.R.id.message);
        log("message " + (message == null ? "none" : message.getText()));
        TextView title = (TextView) byName(d, "alertTitle");
        log("title " + (title == null ? "none" : title.getText()));
        log("positive " + button(d, DialogInterface.BUTTON_POSITIVE));
        log("negative " + button(d, DialogInterface.BUTTON_NEGATIVE));
        log("neutral " + button(d, DialogInterface.BUTTON_NEUTRAL));
        log("click negative:");
        d.getButton(DialogInterface.BUTTON_NEGATIVE).performClick();
        log("after click: showing " + d.isShowing());
        settle(() -> items());
    }

    private void items() {
        CharSequence[] names = {"one", "two", "three"};
        AlertDialog d = new AlertDialog.Builder(this).setTitle("Items").setItems(names, clicks("items")).create();
        d.show();
        ListView list = d.getListView();
        log("items list " + (list != null) + ", count " + (list == null ? -1 : list.getCount())
                + ", first " + (list == null ? "" : list.getItemAtPosition(0)));
        log("click item 1:");
        list.performItemClick(list.getChildAt(1), 1, list.getItemIdAtPosition(1));
        log("after item: showing " + d.isShowing());
        settle(this::single);
    }

    private void single() {
        CharSequence[] names = {"a", "b", "c"};
        AlertDialog d = new AlertDialog.Builder(this).setTitle("Single")
                .setSingleChoiceItems(names, 1, clicks("single"))
                .setPositiveButton("OK", clicks("single-ok")).create();
        d.show();
        ListView list = d.getListView();
        log("single checked " + list.getCheckedItemPosition() + ", choice mode " + list.getChoiceMode());
        list.performItemClick(list.getChildAt(2), 2, list.getItemIdAtPosition(2));
        log("single checked now " + list.getCheckedItemPosition() + ", showing " + d.isShowing());
        d.dismiss();
        settle(this::multi);
    }

    private void multi() {
        CharSequence[] names = {"x", "y", "z"};
        boolean[] checked = {true, false, true};
        AlertDialog d = new AlertDialog.Builder(this).setTitle("Multi")
                .setMultiChoiceItems(names, checked,
                        (dialog, which, isChecked) -> log("multi onClick which=" + which + " checked=" + isChecked))
                .create();
        d.show();
        ListView list = d.getListView();
        log("multi choice mode " + list.getChoiceMode() + ", checked " + list.isItemChecked(0) + list.isItemChecked(1)
                + list.isItemChecked(2));
        list.performItemClick(list.getChildAt(1), 1, list.getItemIdAtPosition(1));
        log("multi array now " + Arrays.toString(checked));
        d.dismiss();
        settle(this::custom);
    }

    private void custom() {
        EditText input = new EditText(this);
        input.setId(777);
        input.setText("typed");
        AlertDialog d = new AlertDialog.Builder(this).setTitle("View").setView(input).create();
        d.show();
        log("custom view found " + (d.findViewById(777) == input) + ", text " + ((EditText) d.findViewById(777)).getText());
        d.dismiss();
        settle(this::late);
    }

    private void late() {
        AlertDialog d = new AlertDialog.Builder(this).create();
        d.setTitle("Late title");
        d.setMessage("Late message");
        d.setButton(DialogInterface.BUTTON_POSITIVE, "Go", clicks("late"));
        d.setCancelable(true);
        d.setOnCancelListener(x -> log("late cancel-listener"));
        d.show();
        log("late message " + ((TextView) d.findViewById(android.R.id.message)).getText());
        log("late positive " + button(d, DialogInterface.BUTTON_POSITIVE));
        log("late negative " + button(d, DialogInterface.BUTTON_NEGATIVE));
        d.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
        d.cancel();
        log("after cancel: showing " + d.isShowing());
        settle(() -> {
            log("built twice: " + outcome(() -> new AlertDialog.Builder(this).setMessage("m").show().isShowing()));
            settle(() -> log("done"));
        });
    }
}
