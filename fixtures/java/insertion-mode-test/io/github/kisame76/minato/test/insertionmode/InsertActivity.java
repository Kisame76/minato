package io.github.kisame76.minato.test.insertionmode;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * The activity of insertion-mode-test: an EditText of a few words, with a text on the clipboard, and the way the
 * cursor's insertion mode starts: a tap in the empty space after the text puts the cursor there and shows the insertion
 * handle, a tap on the handle starts the insertion's action mode, and so does a long press in the empty space. The
 * app's custom insertion callback logs what it is given (the type and the menu), and the activity what it hears of
 * it; each mode is finished as soon as it has started. It says where to tap once its window has the focus ("ready:
 * tap at x,y", "ready: handle at x,y" and "ready: long press at x,y") and ends with "done" after the second mode.
 */
public class InsertActivity extends Activity {
    private static final String TAG = "insertion-mode-test";
    private EditText mEdit;
    private boolean mReady;
    private int mModes;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("insertion-mode-test", "pasted"));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        // The window is edge to edge from target 35 on: the field below the status bar's area.
        root.setPadding(0, 300, 0, 0);
        mEdit = new EditText(this);
        mEdit.setText("alpha beta gamma");
        mEdit.setTextSize(24);
        mEdit.setShowSoftInputOnFocus(false);
        mEdit.setCustomInsertionActionModeCallback(new ActionMode.Callback() {
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
                int length = mEdit.length();
                float density = getResources().getDisplayMetrics().density;
                int y = at[1] + mEdit.getTotalPaddingTop() + (mEdit.getLayout().getLineTop(0)
                        + mEdit.getLayout().getLineBottom(0)) / 2;
                int x = at[0] + mEdit.getWidth() - Math.round(48 * density);
                // The handle hangs below the cursor, at the end of the text, with its center on it.
                int handleX = at[0] + mEdit.getTotalPaddingLeft()
                        + Math.round(mEdit.getLayout().getPrimaryHorizontal(length));
                int handleY = at[1] + mEdit.getTotalPaddingTop() + mEdit.getLayout().getLineBottom(0)
                        + Math.round(12 * density);
                Log.i(TAG, "ready: tap at " + x + "," + y);
                Log.i(TAG, "ready: handle at " + handleX + "," + handleY);
                Log.i(TAG, "ready: long press at " + x + "," + y);
            }
        });
    }

    @Override
    public void onActionModeStarted(ActionMode mode) {
        super.onActionModeStarted(mode);
        Log.i(TAG, "onActionModeStarted type " + mode.getType() + ", selection " + mEdit.getSelectionStart() + "-"
                + mEdit.getSelectionEnd());
        mEdit.post(new Runnable() {
            @Override
            public void run() {
                mode.finish();
            }
        });
    }

    @Override
    public void onActionModeFinished(ActionMode mode) {
        super.onActionModeFinished(mode);
        Log.i(TAG, "onActionModeFinished");
        if (++mModes == 2) {
            mEdit.postDelayed(new Runnable() {
                @Override
                public void run() {
                    Log.i(TAG, "done");
                }
            }, 500);
        }
    }
}
