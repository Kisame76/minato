// Hidden members of the public API's compile-only stub (ADR-0041 a)), which tools/managergen joins with it:
// DisplayManagerGlobal makes a virtual display with its package's constructor.
class VirtualDisplay {
    /** @hide */
    VirtualDisplay(DisplayManagerGlobal global, android.view.Display display, IVirtualDisplayCallback token,
            android.view.Surface surface) {
        throw new RuntimeException("stub");
    }
}
