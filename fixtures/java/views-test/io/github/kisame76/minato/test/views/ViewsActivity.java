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
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * ADR-0043's test 1 (views-test): views drawn in software (android:hardwareAccelerated="false") in colors of the
 * test's own, in a column in the middle of a window that is edge to edge with the system bars shown. A label view
 * draws its text with Canvas.drawText, a plain view is a block of color, a third view draws a Bitmap of density 160,
 * which the window canvas's density scales (step c3), and below them a TextView in a color of the test's own (step
 * d)), and a Button of the theme, whose background is a ripple (test 1 complete). The app logs its lifecycle, the
 * views' bounds in the window and the text's advance once laid out, and the window's insets under views-test-device.
 * When the window gains the focus, which Android gives after the window's first draw, the label's text changes, which
 * redraws the label alone. Then a tap on the Button, which the script plays (adb shell input tap on the AVD, the
 * session's input channel on the Mac), clicks it: "button clicked", and under views-test-device whether a touch came
 * before the click. A run without a tap within 5 s of the text's change clicks the Button with performClick, for a
 * host that plays no touches (java-start-check.sh on the iPhone), which the device line names. A second after the
 * click, when the ripple has gone, the app redraws the Button alone and logs the last line, "done", after that draw.
 */
public class ViewsActivity extends Activity {
    // The application's label, views-test or its twin in the default theme (views-test-default), tags the lines.
    private String TAG;
    private String DEVICE_TAG;

    private LinearLayout mColumn;
    private LabelView mLabel;
    private boolean mLaidOut;
    private boolean mChanged;
    private Button mButton;
    private final java.util.ArrayList<TextView> mScripts = new java.util.ArrayList<>();
    private final java.util.ArrayList<String> mScriptNames = new java.util.ArrayList<>();
    private boolean mTouched;
    private boolean mClicked;
    private boolean mSettled;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        DEVICE_TAG = TAG + "-device";
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
        mButton = new Button(this);
        mButton.setText("Minato button");
        mButton.setOnTouchListener((view, event) -> {
            mTouched = true;
            return false;
        });
        mButton.setOnClickListener(view -> clicked());
        params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(24);
        mColumn.addView(mButton, params);
        if (TAG.endsWith("-fonts")) {
            // views-test-fonts: the scripts and the emoji that the system fonts of Android 15 cover (Hebrew, Arabic,
            // Thai, Devanagari, colour emoji), each in a TextView of its own in the default typeface, below the Button
            // where the bounds above stay.
            final String[] names = {"hebrew", "arabic", "thai", "hindi", "emoji"};
            final String[] texts = {"\u05E9\u05DC\u05D5\u05DD", "\u0645\u0631\u062D\u0628\u0627",
                "\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35", "\u0928\u092E\u0938\u094D\u0924\u0947",
                "\uD83D\uDE00\uD83C\uDF89\uD83D\uDC31"};
            for (int i = 0; i < names.length; i++) {
                TextView script = new TextView(this);
                script.setText(texts[i]);
                script.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
                script.setTextColor(0xFF1A2B3C);
                params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT);
                params.topMargin = dp(8);
                mColumn.addView(script, params);
                mScripts.add(script);
                mScriptNames.add(names[i]);
            }
        }
        if (TAG.endsWith("-bidi")) {
            // views-test-bidi: one TextView of three lines, a Hebrew and an Arabic word in a right-to-left paragraph, a
            // Thai and a Devanagari word, and three emoji, as a chat or name field shows them. AOSP's performTraversals
            // measures the window twice before the first layout (it applies the insets after the first measure), and
            // the second measure of this TextView, which has a layout by then, takes its width from the bounding box
            // of its drawing: wider than its lines. The log shows both measures and the width.
            TextView mixed = new TextView(this) {
                @Override
                protected void onMeasure(int widthSpec, int heightSpec) {
                    final boolean had = getLayout() != null;
                    final int before = had ? getLayout().getWidth() : -1;
                    super.onMeasure(widthSpec, heightSpec);
                    Log.i(TAG, "mixed measure spec " + MeasureSpec.toString(widthSpec) + " x " + MeasureSpec.toString(heightSpec) + ", layout before " + before
                            + ", measured " + getMeasuredWidth() + ", layout after " + getLayout().getWidth());
                }
            };
            mixed.setText("\u05E9\u05DC\u05D5\u05DD \u0645\u0631\u062D\u0628\u0627\n"
                    + "\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35 \u0928\u092E\u0938\u094D\u0924\u0947\n"
                    + "\uD83D\uDE00\uD83C\uDF89\uD83D\uDC31");
            mixed.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            mixed.setTextColor(0xFF1A2B3C);
            params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            params.topMargin = dp(8);
            mColumn.addView(mixed, params);
            mScripts.add(mixed);
            mScriptNames.add("mixed");
        }
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
            mColumn.postDelayed(() -> {
                if (!mClicked) {
                    mButton.performClick();
                }
            }, 5000);
        }
    }

    /** The click: its line, then a redraw a second later, when the ripple has gone, after which "done" follows. */
    private void clicked() {
        if (mClicked) {
            return;
        }
        mClicked = true;
        Log.i(TAG, "button clicked");
        Log.i(DEVICE_TAG, mTouched ? "click from a touch" : "click without a touch (performClick)");
        mColumn.postDelayed(() -> {
            mSettled = true;
            mButton.invalidate();
        }, 1000);
    }

    /** The first layout's bounds and the text's advance. */
    private void laidOut() {
        if (mLaidOut || mLabel.getWidth() == 0) {
            return;
        }
        mLaidOut = true;
        Log.i(TAG, "label bounds " + bounds(mLabel));
        Log.i(TAG, "block bounds " + bounds(mColumn.getChildAt(1)));
        Log.i(TAG, "bitmap bounds " + bounds(mColumn.getChildAt(2)));
        Log.i(TAG, "textview bounds " + bounds(mColumn.getChildAt(3)));
        Log.i(TAG, "button bounds " + bounds(mButton));
        Log.i(TAG, "text advance " + mLabel.advance());
        for (int i = 0; i < mScripts.size(); i++) {
            // views-test-fonts: where each script's TextView is laid out, and its widths.
            final TextView script = mScripts.get(i);
            Log.i(TAG, mScriptNames.get(i) + " bounds " + bounds(script));
            Log.i(TAG, mScriptNames.get(i) + " desired "
                    + android.text.Layout.getDesiredWidth(script.getText(), script.getPaint()) + ", layout width "
                    + script.getLayout().getWidth() + ", line width " + script.getLayout().getLineWidth(0));
            if (mScriptNames.get(i).equals("mixed")) {
              try {
                final android.text.Layout layout = script.getLayout();
                Log.i(TAG, "mixed use bounds " + script.getUseBoundsForWidth() + ", box "
                        + layout.computeDrawingBoundingBox());
                for (int line = 0; line < layout.getLineCount(); line++) {
                    Log.i(TAG, "mixed line " + line + " left " + layout.getLineLeft(line) + " right "
                            + layout.getLineRight(line) + " max " + layout.getLineMax(line));
                }
              } catch (Throwable t) {
                Log.i(TAG, "mixed failed " + t);
              }
            }
        }
    }

    /** The window's insets, once the bars have settled. */
    private void logInsets() {
        Insets bars = getWindow().getDecorView().getRootWindowInsets().getInsets(WindowInsets.Type.systemBars());
        // The rows the screen's comparison starts at: below the status bar, and below the action bar that views-test's
        // twin in the default theme has over its views (the bar's own pixels follow the status bar's inset).
        int top = bars.top;
        final View bar = getWindow().getDecorView().findViewById(
                getResources().getIdentifier("action_bar_container", "id", "android"));
        if (bar != null && bar.getVisibility() == View.VISIBLE) {
            final int[] at = new int[2];
            bar.getLocationInWindow(at);
            top = Math.max(top, at[1] + bar.getHeight());
        }
        Log.i(DEVICE_TAG, "insets top " + top + " bottom " + bars.bottom);
    }

    /** The first draw after the click has settled: "done" once it is posted. */
    private void drawing() {
        if (mSettled && !mDone) {
            mDone = true;
            logInsets();
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
