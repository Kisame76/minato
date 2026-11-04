package io.github.kisame76.minato.test.selectionmode;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * The activity of selection-mode-test: an EditText of a few words, and the way a long press on a word starts the
 * selection's action mode: the app's custom callback logs what it is given (the type, the menu, the selection), and the
 * activity what it hears of it. It says where to long-press once its window has the focus ("ready: long press at
 * x,y"), finishes the mode a second after it started and ends with "done".
 */
public class SelectActivity extends Activity {
    private static final String TAG = "selection-mode-test";
    private EditText mEdit;
    private boolean mReady;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        // The window is edge to edge from target 35 on: the field below the status bar's area.
        root.setPadding(0, 300, 0, 0);
        mEdit = new EditText(this);
        mEdit.setText("alpha beta gamma");
        mEdit.setTextSize(24);
        // No keyboard: a selection in an editable field shows it, which the Mac side feeds (a finding of its own).
        mEdit.setShowSoftInputOnFocus(false);
        mEdit.setCustomSelectionActionModeCallback(new ActionMode.Callback() {
            @Override
            public boolean onCreateActionMode(ActionMode mode, Menu menu) {
                StringBuilder items = new StringBuilder();
                for (int i = 0; i < menu.size(); i++) {
                    items.append(i == 0 ? "" : ", ").append(menu.getItem(i).getTitle());
                }
                Log.i(TAG, "onCreateActionMode type " + mode.getType() + ", menu [" + items + "]");
                return true;
            }

            @Override
            public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
                Log.i(TAG, "onPrepareActionMode");
                return true;
            }

            @Override
            public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
                return false;
            }

            @Override
            public void onDestroyActionMode(ActionMode mode) {
                Log.i(TAG, "onDestroyActionMode");
            }
        });
        root.addView(mEdit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        Log.i(TAG, "onCreate");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mReady) {
            return;
        }
        mReady = true;
        mEdit.post(new Runnable() {
            @Override
            public void run() {
                int[] at = new int[2];
                mEdit.getLocationOnScreen(at);
                // The middle of "beta": the layout's x of its first and last character.
                float x = (mEdit.getLayout().getPrimaryHorizontal(6) + mEdit.getLayout().getPrimaryHorizontal(10)) / 2;
                int y = at[1] + (mEdit.getLayout().getLineTop(0) + mEdit.getLayout().getLineBottom(0)) / 2;
                Log.i(TAG, "ready: long press at " + Math.round(at[0] + mEdit.getTotalPaddingLeft() + x) + "," + y);
            }
        });
    }

    @Override
    public void onActionModeStarted(ActionMode mode) {
        super.onActionModeStarted(mode);
        Log.i(TAG, "onActionModeStarted type " + mode.getType() + ", selection " + mEdit.getSelectionStart() + "-"
                + mEdit.getSelectionEnd());
        mEdit.postDelayed(new Runnable() {
            @Override
            public void run() {
                mode.finish();
            }
        }, 1000);
    }

    @Override
    public void onActionModeFinished(ActionMode mode) {
        super.onActionModeFinished(mode);
        Log.i(TAG, "onActionModeFinished");
        mEdit.postDelayed(new Runnable() {
            @Override
            public void run() {
                Log.i(TAG, "done");
            }
        }, 500);
    }
}
