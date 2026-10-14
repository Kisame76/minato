package io.github.kisame76.minato.test.secondwindows;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;

/**
 * Second windows (ADR-0041 gap 4 (b)): an AlertDialog and PopupWindows in windows of their own, with their frames and
 * callbacks, and what a bad token meets. It runs on the main thread when the launcher activity first has the window
 * focus, a step every SETTLE_MS, so that each window had its traversal. Frames are compared as sizes and as places
 * against their anchor or the screen's width; a window's place on the screen's height depends on the system bars'
 * insets, so it goes to the -device tag, which the comparison leaves out.
 *
 * <ul>
 *   <li>b1: the dialog shown and dismissed, a popup on the dialog's button and one below the activity's button, a
 *       popup with a line wider than config_prefDialogWidth, a dialog on the application context, a popup on a
 *       detached anchor, a sub-window on a dismissed dialog's token and a dialog of a finished activity.</li>
 * </ul>
 */
public class SecondWindowsActivity extends Activity {
    static final String TAG = "second-windows-test";
    private static final int SETTLE_MS = 600;
    /** Wider than config_prefDialogWidth (320 dp) on one line, so that a WRAP_CONTENT window wraps it there. */
    private static final String LONG = "A popup's line long enough to be wider than the preferred width of a dialog, "
            + "which a window that wraps its content is measured at first";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mStep;
    private Button mAnchor;
    private AlertDialog mDialog;
    private View mDialogDecor;
    private PopupWindow mPopup;
    private IBinder mDialogToken;

    /** Compared with Android's log. */
    static void log(String line) {
        Log.i(TAG, line);
    }

    /** Only shown: what depends on the device's system bars. */
    static void logDevice(String line) {
        Log.i(TAG + "-device", line);
    }

    interface Call {
        Object run() throws Exception;
    }

    /** What a call answers or throws, with a token's identity left out of the message. */
    static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            Log.i(TAG + "-device", "thrown", e);
            String message = String.valueOf(e.getMessage()).replaceAll("token (?!null )\\S+ is", "token <token> is");
            return e.getClass().getName() + ": " + message;
        }
    }

    private static int[] screen(View view) {
        int[] location = new int[2];
        view.getLocationOnScreen(location);
        return location;
    }

    private PopupWindow popup(String text) {
        TextView content = new TextView(this);
        content.setText(text);
        content.setPadding(12, 12, 12, 12);
        PopupWindow popup = new PopupWindow(content, ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        popup.setOnDismissListener(() -> log("popup " + text + " dismissed"));
        return popup;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        mAnchor = new Button(this);
        mAnchor.setText("Anchor");
        root.addView(mAnchor, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            next();
        }
    }

    private void next() {
        mHandler.postDelayed(() -> step(++mStep), SETTLE_MS);
    }

    private void step(int step) {
        switch (step) {
            case 1: {
                View decor = getWindow().getDecorView();
                int[] anchor = screen(mAnchor);
                log("activity decor " + decor.getWidth() + "x" + decor.getHeight() + ", anchor at " + anchor[0] + ","
                        + anchor[1] + " size " + mAnchor.getWidth() + "x" + mAnchor.getHeight());
                mDialog = new AlertDialog.Builder(this).setTitle("Title").setMessage("Message")
                        .setPositiveButton("OK", null).setNegativeButton("Cancel", null).create();
                mDialog.setOnShowListener(d -> log("dialog shown"));
                mDialog.setOnDismissListener(d -> log("dialog dismissed"));
                mDialog.show();
                WindowManager.LayoutParams params = mDialog.getWindow().getAttributes();
                log("dialog showing " + mDialog.isShowing() + ", type " + params.type + ", activity's token "
                        + (params.token == decor.getApplicationWindowToken()) + ", dim behind "
                        + ((params.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0) + " " + params.dimAmount);
                next();
                break;
            }
            case 2: {
                mDialogDecor = mDialog.getWindow().getDecorView();
                int[] at = screen(mDialogDecor);
                int screenWidth = getWindow().getDecorView().getWidth();
                log("dialog decor attached " + mDialogDecor.isAttachedToWindow() + ", size "
                        + mDialogDecor.getWidth() + "x" + mDialogDecor.getHeight() + ", left " + at[0] + ", centered "
                        + (at[0] == (screenWidth - mDialogDecor.getWidth()) / 2) + ", window token "
                        + (mDialogDecor.getWindowToken() != getWindow().getDecorView().getWindowToken()));
                logDevice("dialog decor top " + at[1]);
                Button ok = mDialog.getButton(AlertDialog.BUTTON_POSITIVE);
                int[] okAt = screen(ok);
                log("dialog ok at " + (okAt[0] - at[0]) + "," + (okAt[1] - at[1]) + " in the dialog, size "
                        + ok.getWidth() + "x" + ok.getHeight());
                mPopup = popup("inner");
                mPopup.showAsDropDown(ok);
                next();
                break;
            }
            case 3: {
                View content = mPopup.getContentView();
                Button ok = mDialog.getButton(AlertDialog.BUTTON_POSITIVE);
                int[] at = screen(content);
                int[] okAt = screen(ok);
                log("popup inner showing " + mPopup.isShowing() + ", attached " + content.isAttachedToWindow()
                        + ", at " + (at[0] - okAt[0]) + "," + (at[1] - okAt[1] - ok.getHeight())
                        + " from the button's bottom left, size " + content.getWidth() + "x" + content.getHeight()
                        + ", above " + mPopup.isAboveAnchor() + ", own window "
                        + (content.getWindowToken() != mDialogDecor.getWindowToken()));
                mPopup.dismiss();
                mDialogToken = mDialogDecor.getWindowToken();
                mDialog.dismiss();
                next();
                break;
            }
            case 4: {
                log("dialog decor attached " + mDialogDecor.isAttachedToWindow() + ", showing " + mDialog.isShowing());
                mPopup = popup("outer");
                mPopup.showAsDropDown(mAnchor);
                next();
                break;
            }
            case 5: {
                View content = mPopup.getContentView();
                int[] at = screen(content);
                int[] anchor = screen(mAnchor);
                log("popup outer showing " + mPopup.isShowing() + ", attached " + content.isAttachedToWindow()
                        + ", at " + (at[0] - anchor[0]) + "," + (at[1] - anchor[1] - mAnchor.getHeight())
                        + " from the anchor's bottom left, size " + content.getWidth() + "x" + content.getHeight()
                        + ", above " + mPopup.isAboveAnchor());
                mPopup.dismiss();
                mPopup = popup("long");
                ((TextView) mPopup.getContentView()).setText(LONG);
                mPopup.showAsDropDown(mAnchor);
                next();
                break;
            }
            case 6: {
                View content = mPopup.getContentView();
                log("popup long size " + content.getWidth() + "x" + content.getHeight() + ", lines "
                        + ((TextView) content).getLineCount());
                mPopup.dismiss();
                next();
                break;
            }
            case 7: {
                log("application context: " + outcome(() -> {
                    Dialog dialog = new Dialog(getApplicationContext());
                    dialog.show();
                    boolean showing = dialog.isShowing();
                    dialog.dismiss();
                    return "showing " + showing;
                }));
                log("detached anchor: " + outcome(() -> {
                    PopupWindow popup = popup("detached");
                    popup.showAsDropDown(new View(this));
                    boolean showing = popup.isShowing();
                    popup.dismiss();
                    return "showing " + showing;
                }));
                log("dismissed dialog's token: " + outcome(() -> {
                    WindowManager.LayoutParams params =
                            new WindowManager.LayoutParams(WindowManager.LayoutParams.TYPE_APPLICATION_PANEL);
                    params.token = mDialogToken;
                    View view = new View(this);
                    getWindowManager().addView(view, params);
                    getWindowManager().removeView(view);
                    return "added";
                }));
                startActivity(new Intent(this, FinishedActivity.class));
                break;
            }
            default:
                break;
        }
    }
}
