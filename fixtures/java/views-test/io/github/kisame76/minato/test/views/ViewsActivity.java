package io.github.kisame76.minato.test.views;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * ADR-0043's test 1 (views-test): views drawn in software (android:hardwareAccelerated="false") in colors of the
 * test's own, in a column in the middle of a window that is edge to edge with the system bars shown. A label view
 * draws its text with Canvas.drawText, a plain view is a block of color, a third view draws a Bitmap of density 160,
 * which the window canvas's density scales (step c3), and below them a TextView in a color of the test's own (step
 * d)); a Button joins with the XML drawables' step, as its theme's background is a ripple. The app logs its
 * lifecycle, the views' bounds in the window and the text's advance once laid out, and the window's insets under
 * views-test-device. When the window gains the focus, which Android gives after the window's first draw, the label's
 * text changes, which redraws the label alone, and the app logs the last line, "done", after that draw.
 */
public class ViewsActivity extends Activity {
    private static final String TAG = "views-test";
    private static final String DEVICE_TAG = "views-test-device";

    private LinearLayout mColumn;
    private LabelView mLabel;
    private boolean mLaidOut;
    private boolean mChanged;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate");
        getWindow().setBackgroundDrawable(new ColorDrawable(0xFFE8EEF4));
        mColumn = new LinearLayout(this);
        mColumn.setOrientation(LinearLayout.VERTICAL);
        mColumn.setGravity(Gravity.CENTER);
        mLabel = new LabelView(this, "Minato views 1");
        mColumn.addView(mLabel, new LinearLayout.LayoutParams(dp(300), dp(56)));
        View block = new View(this);
        block.setBackground(new ColorDrawable(0xFF3355AA));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(300), dp(48));
        params.topMargin = dp(24);
        mColumn.addView(block, params);
        params = new LinearLayout.LayoutParams(dp(32), dp(32));
        params.topMargin = dp(24);
        mColumn.addView(new BitmapView(this), params);
        TextView text = new TextView(this);
        text.setText("Minato text view");
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        text.setTextColor(0xFF1A2B3C);
        params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(24);
        mColumn.addView(text, params);
        setContentView(mColumn);
        mColumn.getViewTreeObserver().addOnGlobalLayoutListener(this::laidOut);
        mColumn.getViewTreeObserver().addOnDrawListener(this::drawing);
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(TAG, "focus " + hasFocus);
        if (hasFocus && !mChanged) {
            mChanged = true;
            mLabel.setText("Minato views 2");
            Log.i(TAG, "text changed");
        }
    }

    /** The first layout's bounds, the text's advance and the window's insets. */
    private void laidOut() {
        if (mLaidOut || mLabel.getWidth() == 0) {
            return;
        }
        mLaidOut = true;
        Log.i(TAG, "label bounds " + bounds(mLabel));
        Log.i(TAG, "block bounds " + bounds(mColumn.getChildAt(1)));
        Log.i(TAG, "bitmap bounds " + bounds(mColumn.getChildAt(2)));
        Log.i(TAG, "textview bounds " + bounds(mColumn.getChildAt(3)));
        Log.i(TAG, "text advance " + mLabel.advance());
        Insets bars = getWindow().getDecorView().getRootWindowInsets().getInsets(WindowInsets.Type.systemBars());
        Log.i(DEVICE_TAG, "insets top " + bars.top + " bottom " + bars.bottom);
    }

    /** The first draw after the text changed: "done" once it is posted. */
    private void drawing() {
        if (mChanged && !mDone) {
            mDone = true;
            mColumn.post(() -> Log.i(TAG, "done"));
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    /** The view's left, top, right and bottom in the window. */
    private static String bounds(View view) {
        int[] location = new int[2];
        view.getLocationInWindow(location);
        return location[0] + " " + location[1] + " " + (location[0] + view.getWidth()) + " "
                + (location[1] + view.getHeight());
    }

    /**
     * A line of text on a color, drawn with Canvas.drawText at 24 sp in the default typeface, 12 dp from the left and
     * centered by the font's metrics; a new text redraws the view alone.
     */
    private static final class LabelView extends View {
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private String mText;

        LabelView(Context context, String text) {
            super(context);
            mText = text;
            setBackground(new ColorDrawable(0xFFFFE0A0));
            mPaint.setColor(0xFF1A2B3C);
            mPaint.setTypeface(Typeface.DEFAULT);
            mPaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 24,
                    context.getResources().getDisplayMetrics()));
        }

        void setText(String text) {
            mText = text;
            invalidate();
        }

        float advance() {
            return mPaint.measureText(mText);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            Paint.FontMetricsInt metrics = mPaint.getFontMetricsInt();
            int baseline = (getHeight() - (metrics.descent - metrics.ascent)) / 2 - metrics.ascent;
            float left = 12 * getResources().getDisplayMetrics().density;
            canvas.drawText(mText, left, baseline, mPaint);
        }
    }

    /**
     * An 8 x 8 pixel checkerboard Bitmap of density 160 drawn with Canvas.drawBitmap, which the window canvas's
     * density scales: three times at 480 dpi.
     */
    private static final class BitmapView extends View {
        private final Bitmap mBitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);

        BitmapView(Context context) {
            super(context);
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    mBitmap.setPixel(x, y, ((x + y) & 1) == 0 ? 0xFF208040 : 0xFFF0C030);
                }
            }
            mBitmap.setDensity(DisplayMetrics.DENSITY_MEDIUM);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawBitmap(mBitmap, 0, 0, null);
        }
    }
}
