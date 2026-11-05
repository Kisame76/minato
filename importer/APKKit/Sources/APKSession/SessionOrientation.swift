/// The orientation game mode holds for a session (ADR-0032, decision 5), or a session that turns with the
/// device (ADR-0038, decisions 1.2 and 1.3).
public enum SessionOrientation: Sendable, Equatable {
    /// Both landscape sides.
    case landscape
    case portrait
    /// The orientation the device has when the session starts.
    case atStart
    /// Portrait and both landscape sides, turning while the app runs.
    case turns

    /// ActivityInfo's CONFIG_ORIENTATION and CONFIG_SCREEN_SIZE.
    static let orientationFlag = 0x0080
    static let screenSizeFlag = 0x0400

    /// For the launcher activity's `android:screenOrientation`, one of ActivityInfo's constants, nil
    /// when it sets none; and its `android:configChanges` and targetSdk. A value that lets Android turn
    /// the activity turns the session when the activity handles the turn itself: `orientation` and
    /// `screenSize`, or `orientation` alone below targetSdk 13, where a turn changes no screen size for it
    /// (PackageParser adds CONFIG_SCREEN_SIZE). Without them Android creates the activity anew, and the
    /// session keeps the orientation at start instead.
    public init(screenOrientation: Int?, configChanges: Int? = nil, targetSdk: Int? = nil) {
        switch screenOrientation {
        // landscape, sensorLandscape, reverseLandscape, userLandscape
        case 0, 6, 8, 11: self = .landscape
        // portrait, sensorPortrait, reversePortrait, userPortrait; nosensor, which Android gives the
        // natural orientation (ROTATION_0), a phone's portrait
        case 1, 7, 9, 12, 5: self = .portrait
        // locked
        case 14: self = .atStart
        // unspecified, user, behind, sensor, fullSensor, fullUser, none, later values
        default:
            let flags = configChanges ?? 0
            let needed = (targetSdk ?? 0) < 13 ? Self.orientationFlag : Self.orientationFlag | Self.screenSizeFlag
            self = flags & needed == needed ? .turns : .atStart
        }
    }

    /// For `setRequestedOrientation`'s value (APKRT_REQUEST_ORIENTATION): landscape or portrait for the values that
    /// force one, as apk-run turns its window; nil for the others, which leave the session as it is.
    public init?(requested screenOrientation: Int) {
        switch screenOrientation {
        // landscape, sensorLandscape, reverseLandscape, userLandscape
        case 0, 6, 8, 11: self = .landscape
        // portrait, sensorPortrait, reversePortrait, userPortrait
        case 1, 7, 9, 12: self = .portrait
        default: return nil
        }
    }

    /// What the window may take; nil for the orientation at start, which is the scene's.
    public var orientations: InterfaceOrientations? {
        switch self {
        case .landscape: .landscape
        case .portrait: .portrait
        case .turns: [.portrait, .landscape]
        case .atStart: nil
        }
    }

    /// Whether a window of this size is laid out in the orientation: wider than high for landscape, not
    /// for portrait, and any laid-out window for the orientation at start, the one the window has, and
    /// for a session that turns.
    public func fits(width: Double, height: Double) -> Bool {
        guard width > 0, height > 0 else { return false }
        switch self {
        case .landscape: return width > height
        case .portrait: return height >= width
        case .atStart, .turns: return true
        }
    }
}
