// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// ContentProvider chains the caller's attribution to its own.
// (ADR-0039 step c2c1) IContentProvider's defaults name the caller.
class AttributionSource {
    /** @hide */
    public AttributionSource(AttributionSource current, AttributionSource next) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public AttributionSource(int uid, String packageName, String attributionTag) {
        throw new RuntimeException("stub");
    }
}
