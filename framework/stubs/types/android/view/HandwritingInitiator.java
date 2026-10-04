package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: ViewRootImpl.getHandwritingInitiator's type, which
 * View's stylus handwriting reaches.
 */
public class HandwritingInitiator {
    public void onDelegateViewFocused( View view) {
        throw new RuntimeException("stub");
    }

    public void onEditorFocused( View view) {
        throw new RuntimeException("stub");
    }

    public void updateHandwritingAreasForView( View view) {
        throw new RuntimeException("stub");
    }
}
