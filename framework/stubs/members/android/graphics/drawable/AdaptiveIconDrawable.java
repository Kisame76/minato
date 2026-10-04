// Hidden members of the public API's compile-only stub (ADR-0039 b3.5a), which tools/managergen joins with it:
// PackageManager's icon helpers read the source drawable's resource.
// (ADR-0039 step c2c2) DrawableInflater makes one for its <adaptive-icon> tag.
class AdaptiveIconDrawable {
    /** @hide */
    AdaptiveIconDrawable() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getSourceDrawableResId() {
        throw new RuntimeException("stub");
    }
}
