// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Build.getSerial names it; TextAppearanceSpan's constructor from a Context asks canLoadUnsafeResources (b3.4b),
// which AOSP declares abstract and the stub gives a body, so that the API's generated subclasses compile; SystemClock
// names the time detector (b3.5a). javac inlines the constants, so they carry AOSP's values.
class Context {
    /** @hide */
    public static final String DEVICE_IDENTIFIERS_SERVICE = "device_identifiers";

    /** @hide */
    public static final String TIME_DETECTOR_SERVICE = "time_detector";

    /** @hide */
    public boolean canLoadUnsafeResources() {
        throw new RuntimeException("stub");
    }
}
