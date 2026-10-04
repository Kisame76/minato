// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// CompatibilityInfo.Translator calls it.
// (ADR-0039 step c2c2) View's and ViewGroup's touch dispatch (step 7's AOSP MotionEvent has them).
// (ADR-0039 step c2c2) InputEventConsistencyVerifier taints events.
// (ADR-0039 step c2c2) InputEvent, now taken with KeyEvent, declares them abstract or calls them.
import android.os.Parcel;

class MotionEvent {
    /** @hide */
    public final void scale(float scale) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final boolean isTouchEvent() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isTargetAccessibilityFocus() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setTargetAccessibilityFocus(boolean targetsFocus) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final boolean isHoverExitPending() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setHoverExitPending(boolean hoverExitPending) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final int getPointerIdBits() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final MotionEvent split(int idBits) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public float getXDispatchLocation(int pointerIndex) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public float getYDispatchLocation(int pointerIndex) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void setTainted(boolean tainted) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public MotionEvent copy() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getId() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getDisplayId() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setDisplayId(int displayId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final boolean isTainted() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static MotionEvent createFromParcelBody(Parcel in) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void cancel() {
        throw new RuntimeException("stub");
    }
}
