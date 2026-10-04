package android.permission;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: PermissionChecker's result codes, which javac
 * inlines with IPermissionChecker.aidl's values.
 */
public class PermissionCheckerManager {
    public static final int PERMISSION_GRANTED = 0;

    public static final int PERMISSION_SOFT_DENIED = 1;

    public static final int PERMISSION_HARD_DENIED = 2;

    public @interface PermissionResult {
    }
}
