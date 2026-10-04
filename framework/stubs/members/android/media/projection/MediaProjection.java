// Hidden members of the public API's compile-only stub (ADR-0041 a)), which tools/managergen joins with it:
// DisplayManagerGlobal hands a projection's binder to the display service for a virtual display.
class MediaProjection {
    /** @hide */
    public android.media.projection.IMediaProjection getProjection() {
        throw new RuntimeException("stub");
    }
}
