// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's and ViewGroup's debug output.
import android.content.Context;

class ViewDebug {
    /** @hide */
    public static final boolean DEBUG_DRAG = false;

    /** @hide */
    public static final boolean DEBUG_POSITIONING = false;

    /** @hide */
    static Object resolveId(Context context, int id) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public interface CanvasProvider {
        android.graphics.Canvas getCanvas(View view, int width, int height);

        android.graphics.Bitmap createBitmap();
    }
}
