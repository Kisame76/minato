package android.provider;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: the names Settings.Config takes from the
 * configuration module's DeviceConfig, which the fetch lacks. javac inlines the constants, so they carry Android 15's
 * values [verified: ConfigInfrastructure at android-15.0.0_r1].
 */
public final class DeviceConfig {
    public static final int SYNC_DISABLED_MODE_NONE = 0;

    public static final int SYNC_DISABLED_MODE_PERSISTENT = 1;

    public static final int SYNC_DISABLED_MODE_UNTIL_REBOOT = 2;

    public static class BadConfigException extends Exception {
        public BadConfigException() {
            throw new RuntimeException("stub");
        }
    }

    public interface MonitorCallback {
        void onNamespaceUpdate(String updatedNamespace);

        void onDeviceConfigAccess(String callingPackage, String namespace);
    }
}
