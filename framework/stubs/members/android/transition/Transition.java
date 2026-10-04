// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// FragmentTransition renames shared elements.
import android.util.ArrayMap;
import android.view.ViewGroup;

class Transition {
    /** @hide */
    public void setNameOverrides(ArrayMap<String, String> overrides) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void pause(ViewGroup sceneRoot) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void resume(ViewGroup sceneRoot) {
        throw new RuntimeException("stub");
    }
}
