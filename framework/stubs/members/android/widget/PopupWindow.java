// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// DecorView shows the action mode popup at the window token.
import android.os.IBinder;
import android.view.View;

class PopupWindow {
    /** @hide */
    public void showAtLocation(View parent, int gravity, int x, int y) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void showAtLocation(IBinder token, int gravity, int x, int y) {
        throw new RuntimeException("stub");
    }
}
