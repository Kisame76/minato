// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's overlay. ViewGroupOverlay's generated constructor needs one.
import android.content.Context;

class ViewOverlay {
    /** @hide */
    ViewOverlay(Context context, View hostView) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    ViewGroup getOverlayView() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    boolean isEmpty() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    ViewOverlay() {
        throw new RuntimeException("stub");
    }
}
