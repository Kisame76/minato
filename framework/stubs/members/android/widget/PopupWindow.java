// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// DecorView shows the action mode popup at the window token. Editor's handles place their popups by the decor view's
// layout parameters (ADR-0043 step d)).
import android.os.IBinder;
import android.view.View;
import android.view.WindowManager;

class PopupWindow {
    /** @hide */
    public void showAtLocation(View parent, int gravity, int x, int y) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void showAtLocation(IBinder token, int gravity, int x, int y) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    protected boolean hasDecorView() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    protected WindowManager.LayoutParams getDecorViewLayoutParams() {
        throw new RuntimeException("stub");
    }
}
