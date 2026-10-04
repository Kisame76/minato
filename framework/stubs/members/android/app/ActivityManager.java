// Hidden members of the public API's compile-only stub (ADR-0039 b3.5b1), which tools/managergen joins with it:
// PackageUtils sizes its file buffer by isLowRamDeviceStatic, in a helper SigningDetails does not call; StrictMode
// asks getService for its violations (ADR-0039 step c1).
class ActivityManager {
    /** @hide */
    public static IActivityManager getService() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static boolean isLowRamDeviceStatic() {
        throw new RuntimeException("stub");
    }
}
