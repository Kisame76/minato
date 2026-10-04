// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View.getWindowId wraps the session's window ID.
import android.os.IBinder;

class WindowId {
    /** @hide */
    public WindowId( IWindowId target) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public WindowId( IBinder target) {
        throw new RuntimeException("stub");
    }
}
