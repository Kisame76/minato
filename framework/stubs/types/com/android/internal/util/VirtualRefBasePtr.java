package com.android.internal.util;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: the graphics classes that hold a native object on the
 * render thread's side (CanvasProperty, RenderNodeAnimator, AnimatedVectorDrawable's animator) make it; its natives
 * are libandroid_runtime's on a device and not ours until an app animates on a render thread.
 */
public final class VirtualRefBasePtr {
    public VirtualRefBasePtr(long ptr) {
        throw new RuntimeException("stub");
    }

    public long get() {
        throw new RuntimeException("stub");
    }

    public void release() {
        throw new RuntimeException("stub");
    }
}
