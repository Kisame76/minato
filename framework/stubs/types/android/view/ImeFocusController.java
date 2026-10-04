package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: ViewRootImpl.getImeFocusController's type, which
 * View's focus changes notify.
 */
public final class ImeFocusController {
    void onViewFocusChanged(View view, boolean hasFocus) {
        throw new RuntimeException("stub");
    }

    void onViewDetachedFromWindow(View view) {
        throw new RuntimeException("stub");
    }

    boolean hasImeFocus() {
        throw new RuntimeException("stub");
    }
}
