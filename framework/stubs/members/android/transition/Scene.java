// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// TransitionManager tracks scenes.
import android.view.ViewGroup;

class Scene {
    /** @hide */
    static void setCurrentScene(ViewGroup sceneRoot, Scene scene) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    boolean isCreatedFromLayoutResource() {
        throw new RuntimeException("stub");
    }
}
