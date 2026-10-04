package android.transition;

import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: SharedElementCallback's snapshots of shared
 * elements, which only an activity's scene transition takes; scene transitions stay compile-only (c2c3's decision 5).
 */
public class TransitionUtils {
    public static Bitmap createDrawableBitmap(Drawable drawable, View hostView) {
        throw new RuntimeException("stub");
    }

    public static Bitmap createViewBitmap(View view, Matrix matrix, RectF bounds, ViewGroup sceneRoot) {
        throw new RuntimeException("stub");
    }
}
