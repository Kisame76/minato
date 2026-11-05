// The written part of SurfaceTexture (the Unity test APK retest 11: a class apps reflect over names it, so it ships as a
// placeholder: every body logs once and throws UnsupportedOperationException (replaced by ADR-0058 b) /
// category 2)), which tools/managergen joins with the API's methods. Before: the hidden members of the public API's
// compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it: a Surface made from a SurfaceTexture,
// which stays compile-only with the GPU pipeline (M4 step 10).
class SurfaceTexture {
    /** @hide */
    public boolean isSingleBuffered() {
        throw Unwritten.placeholder("android.graphics.SurfaceTexture.isSingleBuffered()");
    }
}
