// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// WindowConfiguration names them; SurfaceTexture names the frame rate's annotations (ADR-0043 test 2).
// (ADR-0039 step c2c2) View votes frame rate categories and makes its drag shadow's surface.
class Surface {
    /** @hide */
    public @interface Rotation {
    }

    /** @hide */
    public static String rotationToString(int rotation) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public @interface FrameRateCompatibility {
    }

    /** @hide */
    public @interface ChangeFrameRateStrategy {
    }

    /** @hide */
    public static final int FRAME_RATE_COMPATIBILITY_GTE = 103;

    /** @hide */
    public static final int FRAME_RATE_CATEGORY_NO_PREFERENCE = 1;

    /** @hide */
    public static final int FRAME_RATE_CATEGORY_LOW = 2;

    /** @hide */
    public static final int FRAME_RATE_CATEGORY_NORMAL = 3;

    /** @hide */
    public static final int FRAME_RATE_CATEGORY_HIGH = 5;

    /** @hide */
    public void destroy() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void copyFrom(SurfaceControl other) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public Surface() {
        throw new RuntimeException("stub");
    }
}
