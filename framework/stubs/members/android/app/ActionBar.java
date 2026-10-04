// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Activity drives its action bar.
import android.content.res.Configuration;
import android.view.ActionMode;
import android.view.KeyEvent;

class ActionBar {
    /** @hide */
    public void setDefaultDisplayHomeAsUpEnabled(boolean enabled) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setShowHideAnimationEnabled(boolean enabled) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void onConfigurationChanged(Configuration config) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void dispatchMenuVisibilityChanged(boolean visible) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ActionMode startActionMode(ActionMode.Callback callback) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean openOptionsMenu() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean closeOptionsMenu() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean invalidateOptionsMenu() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean onMenuKeyEvent(KeyEvent event) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean onKeyShortcut(int keyCode, KeyEvent event) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean collapseActionView() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setWindowTitle(CharSequence title) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void onDestroy() {
        throw new RuntimeException("stub");
    }
}
