// Hidden members of the public API's compile-only stub (ADR-0039 b3.5b1), which tools/managergen joins with it:
// PackageUtils sizes its file buffer by isLowRamDeviceStatic, in a helper SigningDetails does not call.
class ActivityManager {
    /** @hide */
    public static boolean isLowRamDeviceStatic() {
        throw new RuntimeException("stub");
    }
}
