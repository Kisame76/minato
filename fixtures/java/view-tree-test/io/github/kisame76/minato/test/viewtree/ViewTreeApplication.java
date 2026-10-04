package io.github.kisame76.minato.test.viewtree;

import android.app.Application;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.View.MeasureSpec;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.FrameLayout;

/**
 * ADR-0039 step c2c2's test: the view tree as an app's code uses it, in Application.onCreate, as there is no activity
 * before step c2c3. It logs, with the tag view-tree-test, a layout inflated with LayoutInflater, measured at the
 * session's screen exactly and at most, and laid out, detached: each child's layout parameters, measured size and
 * place, then what ViewConfiguration answers (the menu key, from the window manager, only shown), the children's
 * backgrounds (a color and an XML drawable) and the scroll bar the layout gives a child. A global layout listener and a
 * View.post on the detached root run only once a window attaches it, so 500 ms later the app logs whether it is
 * attached and ends with "done"; tools/android-reference.sh compares the log with Android 15's.
 */
public class ViewTreeApplication extends Application {
    private static final String TAG = "view-tree-test";
    // The session's screen, the M4 plan's display: the iPhone 15 Pro Max's 1290 x 2796 px at density 3.
    private static final int WIDTH = 1290;
    private static final int HEIGHT = 2796;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            report();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
            Log.i(TAG, "done");
        }
    }

    private void report() {
        Log.i(TAG, "density " + getResources().getDisplayMetrics().density);
        // As the other Java tests, without an R class: the build makes none.
        int tree = getResources().getIdentifier("tree", "layout", getPackageName());
        ViewGroup root = (ViewGroup) LayoutInflater.from(this).inflate(tree, null);
        Log.i(TAG, "inflated " + root.getClass().getName() + " " + name(root) + " with " + root.getChildCount()
                + " children, layout params " + root.getLayoutParams());
        for (int index = 0; index < root.getChildCount(); index++) {
            View child = root.getChildAt(index);
            FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) child.getLayoutParams();
            Log.i(TAG, "child " + name(child) + " " + child.getClass().getName() + " width " + params.width
                    + " height " + params.height + " gravity 0x" + Integer.toHexString(params.gravity)
                    + " margins " + params.leftMargin + " " + params.topMargin + " " + params.rightMargin + " "
                    + params.bottomMargin);
        }

        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> Log.i(TAG, "global layout"));
        Log.i(TAG, "post " + root.post(() -> Log.i(TAG, "posted runnable ran")));

        for (int mode : new int[] {MeasureSpec.EXACTLY, MeasureSpec.AT_MOST}) {
            root.measure(MeasureSpec.makeMeasureSpec(WIDTH, mode), MeasureSpec.makeMeasureSpec(HEIGHT, mode));
            root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
            Log.i(TAG, (mode == MeasureSpec.EXACTLY ? "exactly" : "at most") + ": root " + root.getMeasuredWidth()
                    + " x " + root.getMeasuredHeight() + ", layout requested " + root.isLayoutRequested());
            for (int index = 0; index < root.getChildCount(); index++) {
                View child = root.getChildAt(index);
                Log.i(TAG, "  " + name(child) + " " + child.getMeasuredWidth() + " x " + child.getMeasuredHeight()
                        + " at " + child.getLeft() + " " + child.getTop() + " " + child.getRight() + " "
                        + child.getBottom());
            }
        }

        ViewConfiguration configuration = ViewConfiguration.get(this);
        Log.i(TAG, "touch slop " + configuration.getScaledTouchSlop());
        // A menu key differs by device: the reference AVD has hardware keys, Minato a navigation bar.
        Log.i(TAG + "-device", "permanent menu key " + configuration.hasPermanentMenuKey());
        Log.i(TAG, "box background " + background(root.getChildAt(0)));
        Log.i(TAG, "corner background " + background(root.getChildAt(1)));
        View fill = root.getChildAt(2);
        Log.i(TAG, "fill: vertical scroll bar " + fill.isVerticalScrollBarEnabled() + ", horizontal "
                + fill.isHorizontalScrollBarEnabled() + ", size " + fill.getScrollBarSize() + ", thumb from the theme "
                + background(fill.getVerticalScrollbarThumbDrawable()));
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            Log.i(TAG, "500 ms later: attached " + root.isAttachedToWindow());
            Log.i(TAG, "done");
        }, 500);
    }

    private String background(View view) {
        return background(view.getBackground());
    }

    private String background(Drawable drawable) {
        if (drawable instanceof ColorDrawable) {
            return "ColorDrawable 0x" + Integer.toHexString(((ColorDrawable) drawable).getColor());
        }
        if (drawable instanceof GradientDrawable) {
            GradientDrawable gradient = (GradientDrawable) drawable;
            return "GradientDrawable shape " + gradient.getShape() + " color 0x"
                    + Integer.toHexString(gradient.getColor().getDefaultColor());
        }
        return String.valueOf(drawable);
    }

    private String name(View view) {
        return view.getId() == View.NO_ID ? "(no id)" : getResources().getResourceEntryName(view.getId());
    }
}
