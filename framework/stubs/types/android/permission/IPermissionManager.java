package android.permission;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: the permission service's AIDL interface,
 * which AppGlobals.getPermissionManager returns. Minato checks permissions from the manifest's grants (ADR-0045
 * decision 6), so no IPermissionManager answers: ActivityThread.getPermissionManager gives null.
 */
public interface IPermissionManager {
}
