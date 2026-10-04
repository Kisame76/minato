// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View keeps the content's insets listener that its window gives the decor view, AttachInfo's field.
class Window {
    /** @hide */
    public interface OnContentApplyWindowInsetsListener {
        android.util.Pair<android.graphics.Insets, WindowInsets> onContentApplyWindowInsets(View view,
                WindowInsets insets);
    }
}
