// Hidden members of the public API's compile-only stub (ADR-0041 a), GLES from Java), which tools/managergen joins
// with it: EGLExt.eglDupNativeFenceFDANDROID makes its fence with them.
class SyncFence {
    /** @hide */
    public static SyncFence createEmpty() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static SyncFence create(android.os.ParcelFileDescriptor wrapped) {
        throw new RuntimeException("stub");
    }
}
