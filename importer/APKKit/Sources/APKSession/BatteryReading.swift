/// UIDevice's battery as the runtime takes it (the battery): `apkrt_battery_set`'s
/// level in whole percent, -1 when UIKit does not know it (its -1, as on the Simulator), and its state, UIDevice's
/// `BatteryState` raw value (APKRT_BATTERY_STATE_* in guest_interface.h), unknown for any other.
public struct BatteryReading: Sendable, Equatable {
    public let percent: Int32
    public let state: Int32

    /// `level` is `UIDevice.batteryLevel` (0 to 1, or -1), `state` the raw value of `UIDevice.batteryState`.
    public init(level: Float, state: Int) {
        if level.isNaN || level < 0 {
            percent = -1
        } else {
            percent = Int32((min(level, 1) * 100).rounded())
        }
        self.state = (0...3).contains(state) ? Int32(state) : 0
    }
}
