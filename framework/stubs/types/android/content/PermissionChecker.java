package android.content;

import android.permission.PermissionCheckerManager;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a provider's permission checks for a caller of
 * another process.
 */
public final class PermissionChecker {
    public static final int PERMISSION_GRANTED = PermissionCheckerManager.PERMISSION_GRANTED;

    public static final int PERMISSION_SOFT_DENIED = PermissionCheckerManager.PERMISSION_SOFT_DENIED;

    public static final int PERMISSION_HARD_DENIED = PermissionCheckerManager.PERMISSION_HARD_DENIED;

    public static int checkPermissionForDataDelivery( Context context, String permission, int pid, int uid, String packageName, String attributionTag, String message, boolean startDataDelivery) {
        throw new RuntimeException("stub");
    }

    public static int checkPermissionForDataDelivery( Context context, String permission, int pid, int uid, String packageName, String attributionTag, String message) {
        throw new RuntimeException("stub");
    }

    public static int checkPermissionForDataDeliveryFromDataSource( Context context, String permission, int pid, AttributionSource attributionSource, String message) {
        throw new RuntimeException("stub");
    }

    public static int checkPermissionForDataDelivery( Context context, String permission, int pid, AttributionSource attributionSource, String message) {
        throw new RuntimeException("stub");
    }

    public static int checkPermissionForDataDelivery( Context context, String permission, int pid, AttributionSource attributionSource, String message, boolean startDataDelivery) {
        throw new RuntimeException("stub");
    }

    public static int checkCallingOrSelfPermissionForDataDelivery( Context context, String permission, String callingPackageName, String callingAttributionTag, String message) {
        throw new RuntimeException("stub");
    }

    public static int checkOpForDataDelivery(Context context, String opName, AttributionSource attributionSource,
            String message) {
        throw new RuntimeException("stub");
    }
}
