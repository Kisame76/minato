package android.graphics;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Surface.copyFrom takes a window's buffer queue,
 * which only ViewRootImpl's and SurfaceView's GPU pipeline make; Minato's surfaces come with ADR-0041 a).
 */
public final class BLASTBufferQueue {
    public long mNativeObject;
}
