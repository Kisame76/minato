package android.view;

/**
 * Surfaces on the host's windows (ADR-0043 c2): a window of ADR-0028's registry, by its ID, as a Java {@link Surface}
 * whose native object is a libnativewindow window (ADR-0041 decision 3). ViewRootImpl takes one at
 * NATIVE_WINDOW_CREATED, a SurfaceView one for its layer's window. Any thread may call both: create touches no shared
 * state, and setBuffersGeometry takes the window's own lock, as a render thread's lock and EGL's swap do.
 */
public final class WindowSurface {
    private WindowSurface() {}

    /**
     * A Surface on the host window {@code window} of {@code width} x {@code height} pixels, as NATIVE_WINDOW_CREATED
     * gives them. It holds the window's first reference until {@link Surface#release}. Its format is the window's own,
     * RGBX_8888, until {@link #setBuffersGeometry}; its size follows the host window's.
     *
     * @throws Surface.OutOfResourcesException if no window could be made
     */
    public static native Surface create(long window, int width, int height);

    /**
     * ANativeWindow_setBuffersGeometry on the Surface's window: width and height both 0 for the window's own size,
     * format 0 for the window's own. Answers 0, or -EINVAL for a size with one 0, whose format is still taken, as the
     * NDK function does.
     *
     * @throws IllegalArgumentException if the Surface is released
     */
    public static native int setBuffersGeometry(Surface surface, int width, int height, int format);
}
