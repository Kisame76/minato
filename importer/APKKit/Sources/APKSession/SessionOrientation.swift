/// The orientation game mode holds for a whole session (ADR-0032, decision 5): no rotation while an
/// app runs.
public enum SessionOrientation: Sendable, Equatable {
    /// Both landscape sides.
    case landscape
    case portrait
    /// The orientation the device has when the session starts.
    case atStart

    /// For the launcher activity's `android:screenOrientation`, one of ActivityInfo's constants; nil
    /// when it sets none.
    public init(screenOrientation: Int?) {
        switch screenOrientation {
        // landscape, sensorLandscape, reverseLandscape, userLandscape
        case 0, 6, 8, 11: self = .landscape
        // portrait, sensorPortrait, reversePortrait, userPortrait; nosensor, which Android gives the
        // natural orientation (ROTATION_0), a phone's portrait
        case 1, 7, 9, 12, 5: self = .portrait
        // unspecified, user, behind, sensor, fullSensor, fullUser, locked, none, later values
        default: self = .atStart
        }
    }
}
