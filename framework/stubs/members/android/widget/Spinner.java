// Hidden members of the public API's compile-only stub (the compatibility plan's gap 5, widgets, batch 1), which
// tools/managergen joins with it: AbsSpinner, a class since that batch, has its subclasses lay out their children.
// Spinner stays a stub until its dropdown and dialog windows (gap 4).
class Spinner {
    /** @hide */
    void layout(int delta, boolean animate) {
        throw new RuntimeException("stub");
    }
}
