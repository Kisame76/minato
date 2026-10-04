// Hidden members of the public API's compile-only stub (ADR-0041 a)), which tools/managergen joins with it:
// DisplayManagerGlobal's and Display's signatures and event masks. javac inlines the constants, so they carry AOSP's
// values; DEBUG, a Log.isLoggable call there, is false here.
class DisplayManager {
    /** @hide */
    static final boolean DEBUG = false;

    /** @hide */
    public static final long EVENT_FLAG_DISPLAY_ADDED = 1L << 0;

    /** @hide */
    public static final long EVENT_FLAG_DISPLAY_REMOVED = 1L << 1;

    /** @hide */
    public static final long EVENT_FLAG_DISPLAY_CHANGED = 1L << 2;

    /** @hide */
    public static final long EVENT_FLAG_DISPLAY_BRIGHTNESS = 1L << 3;

    /** @hide */
    public static final long EVENT_FLAG_HDR_SDR_RATIO_CHANGED = 1L << 4;

    /** @hide */
    public static final long EVENT_FLAG_DISPLAY_CONNECTION_CHANGED = 1L << 5;

    /** @hide */
    public @interface EventsMask {}

    /** @hide */
    public @interface SwitchingType {}

    public interface DisplayListener {
        /** @hide */
        default void onDisplayConnected(int displayId) {
        }

        /** @hide */
        default void onDisplayDisconnected(int displayId) {
        }
    }
}
