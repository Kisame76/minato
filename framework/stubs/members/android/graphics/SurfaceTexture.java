// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// a Surface made from a SurfaceTexture, which stays compile-only with the GPU pipeline (M4 step 10).
class SurfaceTexture {
    /** @hide */
    public boolean isSingleBuffered() {
        throw new RuntimeException("stub");
    }
}
