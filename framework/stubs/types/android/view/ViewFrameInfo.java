package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: ViewRootImpl's frame info, which counts the views
 * measured during an input event.
 */
public class ViewFrameInfo {
    public int getAndIncreaseViewMeasuredCount() {
        throw new RuntimeException("stub");
    }
}
