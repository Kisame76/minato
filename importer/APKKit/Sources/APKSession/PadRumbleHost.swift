/// A pad's motors as the host drives them (GCController.haptics through CoreHaptics in the app; a fake in the tests).
@MainActor
public protocol PadHaptics: AnyObject {
    /// Run the strong and the weak motor at these intensities, 0 to 1, until the next call.
    func run(strong: Float, weak: Float)
    func stop()
    /// The pad or the session goes: stop and let the haptic engines go.
    func release()
}

/// Where a pad's motors are, from GCDeviceHaptics.supportedLocalities: a pad with a left and a right handle (an Xbox or
/// a PlayStation pad) has the strong motor in the left grip and the weak one in the right, as evdev's FF_RUMBLE strong
/// and weak magnitudes; any other pad has one place (GCHapticsLocalityDefault), which takes the stronger.
public enum PadMotorLayout: Sendable, Equatable {
    public enum Place: Sendable, Hashable {
        case `default`, leftHandle, rightHandle
    }

    case handles
    case single

    public init(hasLeftHandle: Bool, hasRightHandle: Bool) {
        self = hasLeftHandle && hasRightHandle ? .handles : .single
    }

    /// The intensity for each place that runs.
    public func intensities(strong: Float, weak: Float) -> [Place: Float] {
        switch self {
        case .handles: [.leftHandle: strong, .rightHandle: weak]
        case .single: [.default: max(strong, weak)]
        }
    }
}

/// Every pad's rumble in a session: the vibrations the guest asks of a pad's InputDevice play on the pad's haptics,
/// step by step as Android 15's VibratorInputMapper plays them (PadRumble). A pad with haptics lists Android's two
/// FF_RUMBLE vibrators (0 strong, 1 weak), one without lists none (InputDevice.hasVibrator false). A pad's haptics are
/// made at its first vibration, as GCController.haptics engines cost power, and go when the pad or the session goes.
/// Each call returns the next step's time on the uptime clock (ns), for the host's timer, or nil when none plays.
@MainActor
public final class PadRumbleHost {
    private let makeHaptics: (Int32) -> PadHaptics?
    private var pads: [Int32: PadRumble] = [:]
    private var haptics: [Int32: PadHaptics] = [:]

    public init(makeHaptics: @escaping (Int32) -> PadHaptics?) {
        self.makeHaptics = makeHaptics
    }

    public func add(_ deviceID: Int32, hasHaptics: Bool) {
        if hasHaptics { pads[deviceID] = pads[deviceID] ?? PadRumble() }
    }

    public func vibratorIDs(_ deviceID: Int32) -> [Int32] {
        pads[deviceID] == nil ? [] : [0, 1]
    }

    public func isVibrating(_ deviceID: Int32) -> Bool {
        pads[deviceID]?.isVibrating ?? false
    }

    public func vibrate(_ deviceID: Int32, _ vibration: PadVibration, now: Int64) -> Int64? {
        guard var rumble = pads[deviceID], let motors = motors(deviceID) else { return nextDeadline }
        apply(rumble.vibrate(vibration, now: now), to: motors)
        pads[deviceID] = rumble
        return nextDeadline
    }

    public func cancel(_ deviceID: Int32, token: Int32) -> Int64? {
        guard var rumble = pads[deviceID] else { return nextDeadline }
        if let motors = haptics[deviceID] { apply(rumble.cancel(token: token), to: motors) }
        pads[deviceID] = rumble
        return nextDeadline
    }

    /// The host's timer fired: every pad whose step ended goes on.
    public func advance(now: Int64) -> Int64? {
        for (id, var rumble) in pads {
            guard let motors = haptics[id] else { continue }
            apply(rumble.advance(now: now), to: motors)
            pads[id] = rumble
        }
        return nextDeadline
    }

    /// The app goes to the background: every pad stops; its haptics stay for the return.
    public func stopAll() {
        for (id, var rumble) in pads {
            if let motors = haptics[id] { apply(rumble.stopAll(), to: motors) }
            pads[id] = rumble
        }
    }

    /// The pad went.
    public func remove(_ deviceID: Int32) {
        guard var rumble = pads.removeValue(forKey: deviceID) else { return }
        guard let motors = haptics.removeValue(forKey: deviceID) else { return }
        apply(rumble.stopAll(), to: motors)
        motors.release()
    }

    /// The session ends.
    public func end() {
        for id in Array(pads.keys) { remove(id) }
    }

    private var nextDeadline: Int64? {
        pads.values.compactMap(\.nextStepTime).min()
    }

    private func motors(_ deviceID: Int32) -> PadHaptics? {
        if let motors = haptics[deviceID] { return motors }
        let motors = makeHaptics(deviceID)
        haptics[deviceID] = motors
        return motors
    }

    private func apply(_ commands: [PadRumble.Command], to motors: PadHaptics) {
        for command in commands {
            switch command {
            case .run(let strong, let weak): motors.run(strong: strong, weak: weak)
            case .stop: motors.stop()
            }
        }
    }
}
