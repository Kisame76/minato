package io.github.kisame76.minato.test.title;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.widget.TextView;

/**
 * Window titles, a step every SETTLE_MS from the launcher activity's first focus: a dialog with setTitle, one without,
 * one with FEATURE_NO_TITLE, then a dialog-themed activity (Theme.Material.Light.Dialog) with its label as its title
 * and after setTitle, an activity on Theme.Light, whose title bar has no action bar, and one on Theme.Material.Light,
 * whose title is in its action bar and which has no title bar. For each window: its decor's
 * size, its title view (android.R.id.title), shown or not, with its text and height, and where the content starts in
 * the window, which for a full-screen window depends on the system bars and goes to the -device tag.
 */
public class TitleActivity extends Activity {
    static final String TAG = "title-test";
    static final int SETTLE_MS = 600;
    static TitleActivity sInstance;
    final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private Dialog mDialog;

    static void log(String line) {
        Log.i(TAG, line);
    }

    static void logDevice(String line) {
        Log.i(TAG + "-device", line);
    }

    /** A window's decor, title view and the content's start, as the app sees them. */
    static void describe(String name, Window window, boolean fullScreen) {
        View decor = window.getDecorView();
        TextView title = window.findViewById(android.R.id.title);
        String titleText = title == null ? "none"
                : (title.getVisibility() == View.VISIBLE ? "shown" : title.getVisibility() == View.GONE ? "gone"
                        : "invisible") + " \"" + title.getText() + "\", height " + title.getHeight();
        int[] content = new int[2];
        window.findViewById(android.R.id.content).getLocationInWindow(content);
        log(name + ": decor " + decor.getWidth() + "x" + decor.getHeight() + ", title " + titleText
                + (fullScreen ? "" : ", content at " + content[1]));
        if (fullScreen) {
            logDevice(name + ": content at " + content[1]);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        TextView text = new TextView(this);
        text.setText("Titles");
        setContentView(text);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            next(1);
        }
    }

    void next(int step) {
        mHandler.postDelayed(() -> step(step), SETTLE_MS);
    }

    private Dialog dialog(String text) {
        Dialog dialog = new Dialog(this);
        TextView content = new TextView(this);
        content.setText(text);
        content.setPadding(24, 24, 24, 24);
        dialog.setContentView(content);
        return dialog;
    }

    private void step(int step) {
        switch (step) {
            case 1:
                mDialog = dialog("A dialog with a title");
                mDialog.setTitle("Dialog title");
                mDialog.show();
                next(2);
                break;
            case 2:
                describe("titled dialog", mDialog.getWindow(), false);
                mDialog.setTitle("Changed title");
                next(3);
                break;
            case 3:
                describe("titled dialog after setTitle", mDialog.getWindow(), false);
                mDialog.dismiss();
                mDialog = dialog("A dialog without setTitle");
                mDialog.show();
                next(4);
                break;
            case 4:
                describe("untitled dialog", mDialog.getWindow(), false);
                mDialog.dismiss();
                mDialog = new Dialog(this);
                mDialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
                TextView content = new TextView(this);
                content.setText("A dialog with FEATURE_NO_TITLE");
                content.setPadding(24, 24, 24, 24);
                mDialog.setContentView(content);
                mDialog.setTitle("Not shown");
                mDialog.show();
                next(5);
                break;
            case 5:
                describe("no-title dialog", mDialog.getWindow(), false);
                mDialog.dismiss();
                startActivity(new Intent(this, FloatingTitleActivity.class));
                break;
            case 6:
                startActivity(new Intent(this, ClassicTitleActivity.class));
                break;
            case 7:
                startActivity(new Intent(this, ActionBarTitleActivity.class));
                break;
            case 8:
                log("done");
                break;
            default:
                break;
        }
    }
}
