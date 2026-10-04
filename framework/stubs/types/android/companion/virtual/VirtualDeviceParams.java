package android.companion.virtual;

/**
 * Compile-only stub (ADR-0045, the start's sensors), never in framework.jar: the device policy that
 * SystemSensorManager asks a virtual device about. javac inlines the constants, so they carry AOSP's values.
 */
public final class VirtualDeviceParams {
    public static final int DEVICE_POLICY_DEFAULT = 0;
    public static final int POLICY_TYPE_SENSORS = 0;
}
