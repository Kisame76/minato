package android.app;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowManager.hasWindowExtensionsEnabled asks it
 * whether the device supports multi-window.
 */
public class ActivityTaskManager {
    public static boolean supportsMultiWindow(android.content.Context context) {
        throw new RuntimeException("stub");
    }

    public static IActivityTaskManager getService() {
        throw new RuntimeException("stub");
    }

    public static int getMaxNumPictureInPictureActions(android.content.Context context) {
        throw new RuntimeException("stub");
    }
}
