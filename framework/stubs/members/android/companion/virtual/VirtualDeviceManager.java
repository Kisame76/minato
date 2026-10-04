// Hidden members of the public API's compile-only stub (ADR-0039 b3.5a), which tools/managergen joins with it:
// PackageManager's hasSystemFeature names the default device. javac inlines the constant, so it carries AOSP's value,
// "default:" + Context.DEVICE_ID_DEFAULT.
class VirtualDeviceManager {
    /** @hide */
    public static final String PERSISTENT_DEVICE_ID_DEFAULT = "default:0";

    // SystemSensorManager's, for a virtual device's context (ADR-0045, the start's sensors).
    /** @hide */
    public static final String ACTION_VIRTUAL_DEVICE_REMOVED =
            "android.companion.virtual.action.VIRTUAL_DEVICE_REMOVED";

    /** @hide */
    public static final String EXTRA_VIRTUAL_DEVICE_ID = "android.companion.virtual.extra.VIRTUAL_DEVICE_ID";

    /** @hide */
    public int getDevicePolicy(int deviceId, int policyType) {
        throw new RuntimeException("stub");
    }
}
