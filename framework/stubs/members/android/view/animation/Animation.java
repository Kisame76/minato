// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View runs view animations.
import android.graphics.RectF;
import android.os.Handler;

class Animation {
    /** @hide */
    public void detach() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setListenerHandler(Handler handler) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void getInvalidateRegion(int left, int top, int right, int bottom, RectF invalidate, Transformation transformation) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void initializeInvalidateRegion(int left, int top, int right, int bottom) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean hasAlpha() {
        throw new RuntimeException("stub");
    }
}
