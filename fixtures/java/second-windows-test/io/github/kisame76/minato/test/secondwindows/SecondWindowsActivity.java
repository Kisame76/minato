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
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Supplier;

/**
 * Second windows (ADR-0041 (b)): an AlertDialog and PopupWindows in windows of their own, with their frames and
 * callbacks, their touches, keys and focus, and what a bad token meets. It runs on the main thread when the launcher
 * activity first has the window focus. Frames are compared as sizes and as places against their anchor or the screen's
 * width; a window's place on the screen's height depends on the system bars' insets, so it goes to the -device tag,
 * which the comparison leaves out.
 *
 * <p>The touches part comes first: each of its windows says "ready: ..." once shown, and the host taps it a second
 * later, at places fixed on the display's 1290 x 2796 px (tools/second-windows-check.sh's lifecycle, and
 * tools/android-reference.sh's taps on the gesture AVD after each ready line). Keys are injected into a window
 * (Window.injectInputEvent), which takes them only while it has the window focus, as an app may inject into its own
 * windows only. The windows' focus changes are kept and logged at the next step, as they come asynchronously. The
 * frames part follows, a step every SETTLE_MS, so that each window had its traversal.
 *
 * <ul>
 *   <li>b1: the dialog shown and dismissed, a popup on the dialog's button and one below the activity's button, a
 *       popup with a line wider than config_prefDialogWidth, a dialog on the application context, a popup on a
 *       detached anchor, a sub-window on a dismissed dialog's token and a dialog of a finished activity.</li>
 *   <li>b2: a tap on a dialog's button and its place in it, a tap outside a dialog, which cancels it and never reaches the
 *       activity, an outside tap with a popup that watches outside touches, which dismisses it and clicks the activity's
 *       button, a tap in a focusable popup and one outside it, which dismisses it alone, keys into the activity under a
 *       dialog and BACK into the dialog, and each window's focus around them.</li>
 *   <li>b3, after the dialog of a finished activity: a translucent activity above this one, with a SurfaceView and a
 *       dialog of its own, made opaque and translucent again (setTranslucent, convertToTranslucent), then a floating
 *       one; this activity's lifecycle while they show, and the focus. Without taps, as the Mac's come at fixed
 *       times.</li>
 * </ul>
 */
public class SecondWindowsActivity extends Activity {
    static final String TAG = "second-windows-test";
    private static final int SETTLE_MS = 600;
    /** How long a touches step waits after its window's focus (or show) before it says "ready". */
    private static final int READY_MS = 300;
    /** The touches part's target button, fixed in px, in the display's middle (645, 1398). */
    private static final int TARGET_WIDTH = 300;
    private static final int TARGET_HEIGHT = 150;
    /** The touches part's popups, fixed in px, below the target. */
    private static final int POPUP_WIDTH = 400;
    private static final int POPUP_HEIGHT = 200;
    /** Wider than config_prefDialogWidth (320 dp) on one line, so that a WRAP_CONTENT window wraps it there. */
    private static final String LONG = "A popup's line long enough to be wider than the preferred width of a dialog, "
            + "which a window that wraps its content is measured at first";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mStep;
    private Button mAnchor;
    private Button mTarget;
    private final List<String> mFocus = new ArrayList<>();
    private int mInput;
    private AlertDialog mDialog;
    private View mDialogDecor;
    private PopupWindow mPopup;
    private Runnable mAfterKeyUp;
    private IBinder mDialogToken;
    /** The activity, for the activities of the b3 part. */
    static SecondWindowsActivity sInstance;
    /** Whether its lifecycle is logged: in the b3 part only, so that the parts before keep their lines. */
    private boolean mLifecycle;

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
        sInstance = this;
        FrameLayout root = new FrameLayout(this);
        mAnchor = new Button(this);
        mAnchor.setText("Anchor");
        root.addView(mAnchor, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        mTarget = new Button(this);
        mTarget.setText("Target");
        mTarget.setOnClickListener(v -> log("target clicked"));
        root.addView(mTarget, new FrameLayout.LayoutParams(TARGET_WIDTH, TARGET_HEIGHT, Gravity.CENTER));
        root.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                log("activity touched at " + Math.round(event.getX()) + "," + Math.round(event.getY()));
            }
            return false;
        });
        setContentView(root);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        lifecycle("onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        lifecycle("onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        lifecycle("onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        lifecycle("onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        lifecycle("onStop");
    }

    private void lifecycle(String callback) {
        if (mLifecycle) {
            log("activity " + callback);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        log("activity key down " + KeyEvent.keyCodeToString(keyCode));
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        log("activity key up " + KeyEvent.keyCodeToString(keyCode));
        Runnable after = mAfterKeyUp;
        mAfterKeyUp = null;
        if (after != null) {
            mHandler.post(after);
        }
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!mStarted) {
            if (hasFocus) {
                mStarted = true;
                input(++mInput);
            }
            return;
        }
        mFocus.add("activity " + hasFocus);
    }

    /** A window's focus changes, kept for the next step's line. */
    void watchFocus(String name, View view) {
        view.getViewTreeObserver().addOnWindowFocusChangeListener(
                (ViewTreeObserver.OnWindowFocusChangeListener) hasFocus -> mFocus.add(name + " " + hasFocus));
    }

    /** The focus changes since the last line, in order. */
    /** The focus changes since the last line, without those of `gone`'s window, which Android may give or not. */
    void logFocusWithout(String gone) {
        mFocus.removeIf(change -> change.startsWith(gone + " "));
        logFocus();
    }

    void logFocus() {
        log("focus: " + (mFocus.isEmpty() ? "no change" : String.join(", ", mFocus)));
        mFocus.clear();
    }

    /** Runs `then` READY_MS after `view`'s window has the focus, as the focus comes when the system gives it. */
    private void whenFocused(View view, Runnable then) {
        if (view.hasWindowFocus()) {
            mHandler.postDelayed(then, READY_MS);
        } else {
            mHandler.postDelayed(() -> whenFocused(view, then), 50);
        }
    }

    /** "ready: <what>" once `focused`'s window has the focus, or READY_MS after its show for null. */
    /**
     * Says "ready: <what>, tap at <x>,<y>" once `focused` has the focus, or a while after the show without one: where
     * the host taps next in the display's pixels, from `tap` once the windows are laid out. The scripts tap there, so
     * that the taps land at every display (the device-size plan, step 1).
     */
    private void ready(String what, View focused, Supplier<int[]> tap) {
        Runnable ready = () -> {
            logFocus();
            int[] place = tap.get();
            log("ready: " + what + ", tap at " + place[0] + "," + place[1]);
        };
        if (focused != null) {
            whenFocused(focused, ready);
        } else {
            mHandler.postDelayed(ready, READY_MS);
        }
    }

    /** `view`'s place on the screen moved by (dx, dy), in the display's pixels. */
    private static int[] on(View view, int dx, int dy) {
        int[] place = new int[2];
        view.getLocationOnScreen(place);
        return new int[] {place[0] + dx, place[1] + dy};
    }

    /** The target's middle. */
    private int[] onTarget() {
        return on(mTarget, mTarget.getWidth() / 2, mTarget.getHeight() / 2);
    }

    /** The next step once the activity has the focus back. */
    private void nextInput() {
        whenFocused(mTarget, () -> input(++mInput));
    }

    private AlertDialog inputDialog(String name) {
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Title").setMessage("Message")
                .setPositiveButton("OK", (d, which) -> log(name + " positive"))
                .setNegativeButton("Cancel", null).create();
        dialog.setOnCancelListener(d -> log(name + " canceled"));
        dialog.setOnDismissListener(d -> {
            log(name + " dismissed");
            nextInput();
        });
        dialog.show();
        watchFocus(name, dialog.getWindow().getDecorView());
        return dialog;
    }

    private PopupWindow inputPopup(String name, boolean focusable) {
        View content = new View(this);
        content.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                log(name + " touched at " + Math.round(event.getX()) + "," + Math.round(event.getY()));
            }
            return false;
        });
        content.setOnClickListener(v -> {
            log(name + " clicked");
            ready(name + " again", null, this::onTarget);
        });
        PopupWindow popup = new PopupWindow(content, POPUP_WIDTH, POPUP_HEIGHT, focusable);
        popup.setOutsideTouchable(!focusable);
        popup.setOnDismissListener(() -> {
            log(name + " dismissed");
            nextInput();
        });
        popup.showAsDropDown(mTarget);
        watchFocus(name, content);
        return popup;
    }

    /** Injects a key's down and up into a window, as an app may inject into its own windows. */
    private static void key(android.view.Window window, int keyCode) {
        long now = android.os.SystemClock.uptimeMillis();
        window.injectInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0));
        window.injectInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0));
    }

    /**
     * The touches part, a step at each window's end: a dialog for a tap on its OK button, one for a tap outside it,
     * a popup that watches outside touches for a tap on the target, a focusable one for a tap in it and one on the
     * target, then a dialog for keys. Each ready line names the tap's place (at the M4 plan's display 1077, 1549;
     * 644, 552; 645, 1398; 695, 1573; 645, 1398).
     */
    private void input(int step) {
        switch (step) {
            case 1: {
                AlertDialog dialog = inputDialog("dialog 1");
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnTouchListener((v, event) -> {
                    // Inside the button, not where: the dialog's enter animation moves it on Android while it runs.
                    if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                        log("dialog 1 ok touched inside the button: " + (event.getX() >= 0 && event.getY() >= 0
                                && event.getX() < v.getWidth() && event.getY() < v.getHeight()));
                    }
                    return false;
                });
                View ok = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
                ready("dialog 1", dialog.getWindow().getDecorView(),
                        () -> on(ok, ok.getWidth() / 2, ok.getHeight() / 2));
                break;
            }
            case 2: {
                logFocus();
                // Outside the dialog: halfway between the display's top and the dialog's.
                View decor = inputDialog("dialog 2").getWindow().getDecorView();
                ready("dialog 2", decor, () -> {
                    int[] top = on(decor, decor.getWidth() / 2, 0);
                    return new int[] {top[0], top[1] / 2};
                });
                break;
            }
            case 3:
                logFocus();
                inputPopup("popup 1", false);
                ready("popup 1", null, this::onTarget);
                break;
            case 4: {
                logFocus();
                View content = inputPopup("popup 2", true).getContentView();
                ready("popup 2", content, () -> on(content, 200, 100));
                break;
            }
            case 5: {
                logFocus();
                AlertDialog dialog = inputDialog("dialog 3");
                dialog.setOnKeyListener((d, keyCode, event) -> {
                    log("dialog 3 key " + (event.getAction() == KeyEvent.ACTION_DOWN ? "down " : "up ")
                            + KeyEvent.keyCodeToString(keyCode));
                    return false;
                });
                whenFocused(dialog.getWindow().getDecorView(), () -> {
                    logFocus();
                    // The activity's key goes through its input method stage, whose answer may come after keys
                    // injected later into the dialog: BACK waits for the activity's key up, in one order everywhere.
                    mAfterKeyUp = () -> key(dialog.getWindow(), KeyEvent.KEYCODE_BACK);
                    key(getWindow(), KeyEvent.KEYCODE_A);
                });
                break;
            }
            case 6:
                logFocus();
                key(getWindow(), KeyEvent.KEYCODE_A);
                log("touches part over");
                next();
                break;
            default:
                break;
        }
    }

    /** The b3 part, after the dialog of a finished activity: the translucent activity first. */
    void translucentPart() {
        // The focus changes around the finished activity's dialog, which Android gives in no fixed number.
        mFocus.clear();
        mLifecycle = true;
        startActivity(new Intent(this, TranslucentActivity.class));
    }

    /** TranslucentActivity's, at its destroy: the floating one next. */
    void afterTranslucent(List<String> surface) {
        mHandler.postDelayed(() -> {
            logFocusWithout("translucent");
            log("translucent surface: " + String.join(", ", surface));
            startActivity(new Intent(this, FloatingActivity.class));
        }, SETTLE_MS);
    }

    /** FloatingActivity's, at its destroy: the end. */
    void afterFloating() {
        mHandler.postDelayed(() -> {
            logFocusWithout("floating");
            mLifecycle = false;
            log("done");
        }, SETTLE_MS);
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
