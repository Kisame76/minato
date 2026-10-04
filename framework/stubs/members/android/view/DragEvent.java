// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View and ViewGroup dispatch drag events. View and ViewGroup read the drag position.
import android.content.ClipData;

class DragEvent {
    /** @hide */
    int mAction;

    /** @hide */
    ClipData mClipData;

    /** @hide */
    boolean mEventHandlerWasCalled;

    /** @hide */
    public static DragEvent obtain(DragEvent source) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void recycle() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    float mX;

    /** @hide */
    float mY;
}
