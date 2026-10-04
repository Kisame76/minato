package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View invalidates the ghost a transition overlay
 * keeps.
 */
public class GhostView extends View {
    public GhostView() {
        super(null);
        throw new RuntimeException("stub");
    }

    public static GhostView addGhost(View view, ViewGroup viewGroup, android.graphics.Matrix matrix) {
        throw new RuntimeException("stub");
    }

    public static void removeGhost(View view) {
        throw new RuntimeException("stub");
    }
}
