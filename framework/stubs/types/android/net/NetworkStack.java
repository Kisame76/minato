package android.net;

/**
 * Compile-only stub (the battery), never in framework.jar: the network stack's
 * permission, which an @EnforcePermission of the battery stats' interface (IBatteryStats) names; javac puts the
 * constant in, so nothing loads this class.
 */
public final class NetworkStack {
    public static final String PERMISSION_MAINLINE_NETWORK_STACK = "android.permission.MAINLINE_NETWORK_STACK";
}
