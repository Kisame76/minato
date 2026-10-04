package android.os;

import android.content.Context;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: IDisplayManager's AIDL Stub constructor names it, which no app
 * process runs: the display service is a local ServiceStub (ADR-0045 decision 3). IPlatformCompat's Stub names it too
 * (ADR-0039 step c2e), whose local service is AIDL's Default class as well.
 */
public class PermissionEnforcer {
    public static PermissionEnforcer fromContext(Context context) {
        throw new RuntimeException("stub");
    }

    public void enforcePermission(String permission, int pid, int uid) {
        throw new RuntimeException("stub");
    }

    public void enforcePermissionAllOf(String[] permissions, int pid, int uid) {
        throw new RuntimeException("stub");
    }
}
