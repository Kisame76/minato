package io.github.kisame76.minato.test.keyboardpost;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Ordinary Android views that keep changing while the keyboard is up: a text field with the focus and the keyboard,
 * a line of text the main thread changes about every 16 ms, and a bar an own thread invalidates as often
 * (View.postInvalidate). Every change makes the window draw and post a new picture while the keyboard's window is on
 * screen, and the trips to the background and back give the keyboard's window new layout to do; this is where a
 * picture's change committed off the platform's main thread made it abort. At each resume ("resumed <n>"), once the
 * window has the focus (Android serves a view's keyboard request only then), the field takes the focus and asks for
 * the keyboard ("showSoftInput <result>"), and once the keyboard's inset is there the activity logs "keyboard shown
 * <n>". "frames <n>" comes every 120 changes; "done" comes RUN_MS after the first keyboard, with the
 * number of changes and resumes.
 */
public class PostActivity extends Activity {
    private static final String TAG = "keyboard-post-test";
    /** From the first keyboard to "done": room for the driver's trips to the background. */
    private static final long RUN_MS = 60000;
    private static final long TICK_MS = 16;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private EditText mField;
    private TextView mTicker;
    private BarView mBar;
    private Thread mPoster;
    private volatile boolean mRunning = true;
    private int mTicks;
    private int mResumes;
    private int mShownFor;
    private int mAskedFor;
    private long mDoneAt;

    /** A bar whose width follows a counter that the poster thread steps. */
    private static final class BarView extends View {
        private final Paint mPaint = new Paint();
        volatile int mStep;

        BarView(Activity activity) {
            super(activity);
            mPaint.setColor(Color.rgb(40, 120, 200));
        }

        @Override
        protected void onDraw(Canvas canvas) {
            int width = getWidth() * ((mStep % 100) + 1) / 100;
            canvas.drawRect(0, 0, width, getHeight(), mPaint);
        }
    }

    private final Runnable mTick = new Runnable() {
        @Override
        public void run() {
            if (!mRunning) {
                return;
            }
            mTicks++;
            mTicker.setText("change " + mTicks);
            if (mTicks % 120 == 0) {
                Log.i(TAG, "frames " + mTicks);
            }
            if (mDoneAt != 0 && SystemClock.uptimeMillis() >= mDoneAt) {
                mRunning = false;
                Log.i(TAG, "done: " + mTicks + " changes, " + mResumes + " resumes");
                return;
            }
            mHandler.postDelayed(this, TICK_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        mField = new EditText(this);
        mField.setHint("A field with the keyboard");
        mTicker = new TextView(this);
        mTicker.setTextSize(24);
        mBar = new BarView(this);
        layout.addView(mField, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        layout.addView(mTicker, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        layout.addView(mBar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 48));
        layout.setOnApplyWindowInsetsListener((view, insets) -> {
            if (insets.isVisible(WindowInsets.Type.ime()) && mShownFor != mResumes) {
                mShownFor = mResumes;
                Log.i(TAG, "keyboard shown " + mResumes);
                if (mDoneAt == 0) {
                    mDoneAt = SystemClock.uptimeMillis() + RUN_MS;
                }
            }
            return view.onApplyWindowInsets(insets);
        });
        setContentView(layout);
        mPoster = new Thread(() -> {
            while (mRunning) {
                mBar.mStep++;
                mBar.postInvalidate();
                SystemClock.sleep(TICK_MS);
            }
        }, "keyboard-post-poster");
        mPoster.start();
        mHandler.post(mTick);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mResumes++;
        Log.i(TAG, "resumed " + mResumes);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mAskedFor == mResumes) {
            return;
        }
        mAskedFor = mResumes;
        mField.requestFocus();
        InputMethodManager manager = getSystemService(InputMethodManager.class);
        Log.i(TAG, "showSoftInput " + manager.showSoftInput(mField, InputMethodManager.SHOW_IMPLICIT));
    }

    @Override
    protected void onPause() {
        Log.i(TAG, "paused " + mResumes);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        mRunning = false;
        super.onDestroy();
    }
}
