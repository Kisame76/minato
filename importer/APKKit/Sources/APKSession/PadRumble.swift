/// A vibration an app asks of a game pad, as Android 15's InputManagerService hands it to the input reader: steps of a
/// duration and an amplitude for each of an FF_RUMBLE pad's two vibrators (0 the strong motor, 1 the weak), and the
/// step to start again from at the end, or none. VibrationInfo turns the app's one-shot or waveform into the steps
/// (DEFAULT_AMPLITUDE as 192); nativeVibrate gives a mono effect's amplitude to both vibrators and refuses a pattern of
/// more than MAX_VIBRATE_PATTERN_SIZE (100) steps, and caps a step at 100 s.
public struct PadVibration: Sendable, Equatable {
    public struct Step: Sendable, Equatable {
        public var milliseconds: Int64
        /// 0 to 255, Android's amplitude; 0 is a pause.
        public var strong: UInt8
        public var weak: UInt8
    }

    /// InputManagerService's DEFAULT_VIBRATION_MAGNITUDE, for VibrationEffect.DEFAULT_AMPLITUDE.
    public static let defaultAmplitude: UInt8 = 192
    static let maxSteps = 100
    static let maxStepMilliseconds: Int64 = 100_000

    public let steps: [Step]
    /// The step to start again from at the end, -1 for none.
    public let repeatIndex: Int
    /// The vibration's owner: a cancel ends it only with the same token.
    public let token: Int32

    /// A vibration of both motors alike (CombinedVibration.Mono, Vibrator.vibrate); nil for a pattern that Android
    /// skips or rejects.
    public init?(durations: [Int64], amplitudes: [UInt8], repeatIndex: Int, token: Int32) {
        self.init(durations: durations, strong: amplitudes, weak: amplitudes, repeatIndex: repeatIndex, token: token)
    }

    /// A vibration with each motor's own amplitudes (CombinedVibration.Stereo, VibratorManager.vibrate).
    public init?(durations: [Int64], strong: [UInt8], weak: [UInt8], repeatIndex: Int, token: Int32) {
        guard durations.count <= Self.maxSteps, strong.count == durations.count, weak.count == durations.count,
              repeatIndex < durations.count, repeatIndex >= -1
        else { return nil }
        steps = durations.indices.map {
            Step(milliseconds: min(max(durations[$0], 0), Self.maxStepMilliseconds), strong: strong[$0],
                 weak: weak[$0])
        }
        self.repeatIndex = repeatIndex
        self.token = token
    }
}

/// One pad's vibration player, as Android 15's VibratorInputMapper: a vibration replaces the one that plays and starts
/// at its first step; at each step's end the next one starts, from the repeat index again after the last, else the
/// motors stop; a cancel with the playing vibration's token stops them. A step with both amplitudes 0 stops the motors
/// for its duration (EventHub's cancelVibrate). Times are ns on the uptime clock; the host calls `advance` at
/// `nextStepTime` and hands the commands to the pad's haptics.
public struct PadRumble: Sendable {
    public enum Command: Sendable, Equatable {
        /// Run the strong and the weak motor at these intensities, 0 to 1, until the next command.
        case run(strong: Float, weak: Float)
        case stop
    }

    private var vibration: PadVibration?
    private var index = -1
    public private(set) var nextStepTime: Int64?

    public init() {}

    public var isVibrating: Bool { vibration != nil }

    public mutating func vibrate(_ vibration: PadVibration, now: Int64) -> [Command] {
        self.vibration = vibration
        index = -1
        return nextStep(now: now)
    }

    public mutating func cancel(token: Int32) -> [Command] {
        guard let vibration, vibration.token == token else { return [] }
        return stopVibrating()
    }

    /// The pad goes, the session ends or the app goes to the background: whatever plays stops.
    public mutating func stopAll() -> [Command] {
        vibration == nil ? [] : stopVibrating()
    }

    public mutating func advance(now: Int64) -> [Command] {
        guard vibration != nil, let next = nextStepTime, now >= next else { return [] }
        return nextStep(now: now)
    }

    private mutating func nextStep(now: Int64) -> [Command] {
        guard let vibration else { return [] }
        index += 1
        if index >= vibration.steps.count {
            guard vibration.repeatIndex >= 0 else { return stopVibrating() }
            index = vibration.repeatIndex
        }
        let step = vibration.steps[index]
        nextStepTime = now + step.milliseconds * 1_000_000
        guard step.strong != 0 || step.weak != 0 else { return [.stop] }
        return [.run(strong: Float(step.strong) / 255, weak: Float(step.weak) / 255)]
    }

    private mutating func stopVibrating() -> [Command] {
        vibration = nil
        index = -1
        nextStepTime = nil
        return [.stop]
    }
}
