// Hidden members of the public API's compile-only stub (ADR-0045, the input service), which tools/managergen joins
// with it: the battery status annotation that IInputDeviceBatteryState names.
class BatteryState {
    /** @hide */
    public @interface BatteryStatus {
    }
}
