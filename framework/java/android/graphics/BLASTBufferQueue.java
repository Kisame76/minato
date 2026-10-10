package android.graphics;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11; Grafik owns it, replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c3): Surface.copyFrom takes a window's
 * buffer queue, which only ViewRootImpl's and SurfaceView's GPU pipeline make; Minato's surfaces come with ADR-0041 a).
 */
public final class BLASTBufferQueue {
    public BLASTBufferQueue() {
        throw Unwritten.placeholder("android.graphics.BLASTBufferQueue()");
    }

    public long mNativeObject;
}
