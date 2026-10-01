// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ResourcesProvider calls them.
class OverlayInfo {
    /** @hide */
    public String getBaseCodePath() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isFabricated() {
        throw new RuntimeException("stub");
    }
}
