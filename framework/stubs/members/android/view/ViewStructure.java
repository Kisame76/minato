// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's assist structure; autofill is off.
import android.graphics.Rect;

class ViewStructure {
    /** @hide */
    public abstract void setAssistBlocked(boolean state);

    /** @hide */
    public void setIsCredential(boolean isCredential) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public abstract Rect getTempRect();
}
