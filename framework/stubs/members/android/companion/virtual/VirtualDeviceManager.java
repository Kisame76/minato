// Hidden members of the public API's compile-only stub (ADR-0039 b3.5a), which tools/managergen joins with it:
// PackageManager's hasSystemFeature names the default device. javac inlines the constant, so it carries AOSP's value,
// "default:" + Context.DEVICE_ID_DEFAULT.
class VirtualDeviceManager {
    /** @hide */
    public static final String PERSISTENT_DEVICE_ID_DEFAULT = "default:0";
}
