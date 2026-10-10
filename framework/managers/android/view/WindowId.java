// The written part of WindowId (App, the Unity test APK retest 11: a class apps reflect over names it, so it ships as a
// placeholder: every body logs once and throws UnsupportedOperationException (Grafik owns it, replaced by ADR-0058 b) /
// category 2)), which tools/managergen joins with the API's methods. Before: the hidden members of the public API's
// compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it: View.getWindowId wraps the session's
// window ID.
import android.os.IBinder;

class WindowId {
    /** @hide */
    public WindowId( IWindowId target) {
        throw Unwritten.placeholder("android.view.WindowId(IWindowId)");
    }

    /** @hide */
    public WindowId( IBinder target) {
        throw Unwritten.placeholder("android.view.WindowId(IBinder)");
    }
}
