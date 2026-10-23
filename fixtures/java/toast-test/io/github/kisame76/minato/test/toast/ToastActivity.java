package io.github.kisame76.minato.test.toast;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Toasts (ADR-0041 gap 4 (b4)), a step after each toast's end, from the launcher activity's first window focus: a
 * text toast's view, gravity and offset, and how long a short and a long one show (as a range, which the device's
 * animations stay inside); two toasts in a row, which show one after the other; a cancel; the package's limit of five
 * queued toasts; a toast with a view of its own, its window and its view's frame, and a dialog shown after it, which
 * logs nothing, so that the Mac's check sees the toast above the newer dialog; a toast window the app adds itself
 * without the system's token; and a toast made on a thread without a looper.
 */
public class ToastActivity extends Activity {
    static final String TAG = "toast-test";
    private static final int SETTLE_MS = 600;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private boolean mSixthShown;

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

    /** How long a toast showed, from its show() to onToastHidden. */
    static String length(long ms) {
        return ms < 1900 ? "under 1.9 s" : ms < 3400 ? "short" : ms < 5500 ? "long" : "over 5.5 s";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("Toasts");
        setContentView(text);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.postDelayed(() -> step(1), SETTLE_MS);
        }
    }

    /** A toast whose callback logs its show and its end, the time it showed with `timed`, then runs `then`. */
    private Toast watched(Toast toast, String name, boolean timed, Runnable then) {
        final long start = SystemClock.uptimeMillis();
        toast.addCallback(new Toast.Callback() {
            @Override
            public void onToastShown() {
                log(name + " shown");
            }

            @Override
            public void onToastHidden() {
                log(name + " hidden" + (timed ? ", " + length(SystemClock.uptimeMillis() - start) : ""));
                if (then != null) {
                    mHandler.postDelayed(then, SETTLE_MS);
                }
            }
        });
        return toast;
    }

    private Toast text(String name, int duration, boolean timed, Runnable then) {
        return watched(Toast.makeText(this, "Toast " + name, duration), name, timed, then);
    }

    private void step(int step) {
        switch (step) {
            case 1: {
                Toast toast = text("text 1", Toast.LENGTH_SHORT, true, () -> step(2));
                log("text toast: view " + (toast.getView() == null ? "none" : "set") + ", gravity 0x"
                        + Integer.toHexString(toast.getGravity()) + ", y offset " + toast.getYOffset()
                        + ", duration " + toast.getDuration());
                toast.show();
                break;
            }
            case 2:
                text("text 2", Toast.LENGTH_LONG, true, () -> step(3)).show();
                break;
            case 3:
                text("queue a", Toast.LENGTH_SHORT, false, null).show();
                text("queue b", Toast.LENGTH_SHORT, false, () -> step(4)).show();
                break;
            case 4: {
                final Toast toast = Toast.makeText(this, "Toast cancel", Toast.LENGTH_LONG);
                final long[] canceled = new long[1];
                toast.addCallback(new Toast.Callback() {
                    @Override
                    public void onToastShown() {
                        log("cancel shown");
                        mHandler.postDelayed(() -> {
                            canceled[0] = SystemClock.uptimeMillis();
                            toast.cancel();
                        }, 300);
                    }

                    @Override
                    public void onToastHidden() {
                        log("cancel hidden, " + (SystemClock.uptimeMillis() - canceled[0] < 1000 ? "at once" : "late"));
                        mHandler.postDelayed(() -> step(5), SETTLE_MS);
                    }
                });
                toast.show();
                break;
            }
            case 5:
                for (int i = 1; i <= 6; i++) {
                    final int n = i;
                    Toast toast = text("limit " + n, Toast.LENGTH_SHORT, false,
                            n == 5 ? () -> mHandler.postDelayed(() -> step(6), 1500) : null);
                    if (n == 6) {
                        toast.addCallback(new Toast.Callback() {
                            @Override
                            public void onToastShown() {
                                mSixthShown = true;
                            }
                        });
                    }
                    toast.show();
                }
                break;
            case 6: {
                log("limit: the sixth shown " + mSixthShown);
                final TextView view = new TextView(this);
                view.setText("Custom");
                view.setPadding(24, 24, 24, 24);
                view.setBackgroundColor(Color.DKGRAY);
                view.setTextColor(Color.WHITE);
                final android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(this)
                        .setMessage("Below the toast").create();
                Toast toast = new Toast(this);
                toast.setView(view);
                toast.setDuration(Toast.LENGTH_SHORT);
                toast.addCallback(new Toast.Callback() {
                    @Override
                    public void onToastShown() {
                        dialog.show();
                        mHandler.postDelayed(() -> custom(view), 300);
                    }
                });
                watched(toast, "custom", true, () -> {
                    dialog.dismiss();
                    step(7);
                }).show();
                break;
            }
            case 7:
                log("own toast window: " + outcome(() -> {
                    View view = new View(this);
                    getWindowManager().addView(view,
                            new WindowManager.LayoutParams(WindowManager.LayoutParams.TYPE_TOAST));
                    getWindowManager().removeView(view);
                    return "added";
                }));
                Thread thread = new Thread(() -> log("toast on a thread without a looper: " + outcome(() -> {
                    Toast.makeText(this, "Toast thread", Toast.LENGTH_SHORT);
                    return "made";
                })));
                thread.start();
                try {
                    thread.join();
                } catch (InterruptedException e) {
                    throw new AssertionError(e);
                }
                log("done");
                break;
            default:
                break;
        }
    }

    /** The custom toast's window and its view's frame, once it laid out. */
    private void custom(View view) {
        WindowManager.LayoutParams params = (WindowManager.LayoutParams) view.getLayoutParams();
        int[] at = new int[2];
        view.getLocationOnScreen(at);
        int screenWidth = getWindow().getDecorView().getWidth();
        log("custom view: attached " + view.isAttachedToWindow() + ", type " + params.type + ", not focusable "
                + ((params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) != 0) + ", not touchable "
                + ((params.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) + ", size " + view.getWidth()
                + "x" + view.getHeight() + ", centered " + (at[0] == (screenWidth - view.getWidth()) / 2));
        logDevice("custom view top " + at[1]);
    }
}
