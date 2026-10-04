package io.github.kisame76.minato.test.viewtree;

import android.content.Context;
import android.util.AttributeSet;
import android.util.Log;
import android.widget.FrameLayout;

/**
 * view-tree-test's root, an app's own FrameLayout: ART checks an app class against the boot loader's types of every
 * interface method in its table when it links it (ValidateSuperClassDescriptors), so loading it proves those types
 * real. It logs the measure specs its parent hands it.
 */
public class TreeLayout extends FrameLayout {
    public TreeLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        Log.i("view-tree-test", "TreeLayout.onMeasure " + MeasureSpec.toString(widthMeasureSpec) + ", "
                + MeasureSpec.toString(heightMeasureSpec));
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
    }
}
