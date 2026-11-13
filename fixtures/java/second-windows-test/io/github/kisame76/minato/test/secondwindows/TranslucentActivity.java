package io.github.kisame76.minato.test.secondwindows;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.AlertDialog;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;

/**
 * The b3 part's translucent activity (ADR-0041 (b3)), on Theme.Translucent.NoTitleBar above SecondWindowsActivity,
 * which stays paused below it: a SurfaceView filled by lockCanvas, whose callbacks it keeps for one line at the end, a
 * dialog of its own, a dialog of the activity below, which goes with it and comes back, then setTranslucent(false),
 * convertToTranslucent with a listener, as swipe-back libraries call it, and setTranslucent(true) once translucent
 * again, a step every SETTLE_MS from its first focus, and its finish.
 */
public class TranslucentActivity extends Activity {
    private static final int SETTLE_MS = 600;
    private static final int SURFACE_SIZE = 300;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<String> mSurface = new ArrayList<>();
    private boolean mStarted;
    private AlertDialog mDialog;
    private AlertDialog mBelow;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Canvas canvas = holder.lockCanvas();
                if (canvas != null) {
                    canvas.drawColor(Color.BLUE);
                    holder.unlockCanvasAndPost(canvas);
                }
                mSurface.add("created, canvas " + (canvas != null));
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                mSurface.add("changed " + width + "x" + height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                mSurface.add("destroyed");
            }
        });
        root.addView(surface, new FrameLayout.LayoutParams(SURFACE_SIZE, SURFACE_SIZE, Gravity.TOP | Gravity.START));
        setContentView(root);
        SecondWindowsActivity.sInstance.watchFocus("translucent", root);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.postDelayed(() -> step(1), SETTLE_MS);
        }
    }

    private void step(int step) {
        SecondWindowsActivity main = SecondWindowsActivity.sInstance;
        switch (step) {
            case 1: {
                main.logFocus();
                View decor = getWindow().getDecorView();
                WindowManager.LayoutParams params = getWindow().getAttributes();
                SecondWindowsActivity.log("translucent decor " + decor.getWidth() + "x" + decor.getHeight()
                        + ", type " + params.type + ", own window "
                        + (decor.getWindowToken() != main.getWindow().getDecorView().getWindowToken()));
                mDialog = new AlertDialog.Builder(this).setTitle("Title").setMessage("Message")
                        .setPositiveButton("OK", null).create();
                mDialog.show();
                main.watchFocus("translucent dialog", mDialog.getWindow().getDecorView());
                break;
            }
            case 2: {
                main.logFocus();
                WindowManager.LayoutParams params = mDialog.getWindow().getAttributes();
                SecondWindowsActivity.log("translucent dialog type " + params.type + ", its activity's token "
                        + (params.token == getWindow().getAttributes().token));
                mDialog.dismiss();
                mBelow = new AlertDialog.Builder(main).setTitle("Below").setMessage("Message").create();
                mBelow.show();
                break;
            }
            case 3:
                main.logFocus();
                SecondWindowsActivity.log("activity dialog showing " + mBelow.isShowing());
                SecondWindowsActivity.log("setTranslucent(false): " + setTranslucent(false));
                break;
            case 4:
                SecondWindowsActivity.log("convertToTranslucent: " + SecondWindowsActivity.outcome(() -> {
                    Class<?> listener = Class.forName("android.app.Activity$TranslucentConversionListener");
                    Object callback = Proxy.newProxyInstance(listener.getClassLoader(), new Class<?>[] {listener},
                            (proxy, method, args) -> {
                                if (method.getName().equals("onTranslucentConversionComplete")) {
                                    SecondWindowsActivity.log("translucent conversion complete " + args[0]);
                                }
                                return null;
                            });
                    Method convert = Activity.class.getMethod("convertToTranslucent", listener,
                            ActivityOptions.class);
                    return convert.invoke(this, callback, null);
                }));
                break;
            case 5:
                SecondWindowsActivity.log("setTranslucent(true) when translucent: " + setTranslucent(true));
                SecondWindowsActivity.log("activity dialog showing " + mBelow.isShowing());
                mBelow.dismiss();
                finish();
                return;
            default:
                return;
        }
        mHandler.postDelayed(() -> step(step + 1), SETTLE_MS);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        SecondWindowsActivity.log("translucent destroyed");
        SecondWindowsActivity.sInstance.afterTranslucent(mSurface);
    }
}
