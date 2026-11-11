import APKSession
import CoreHaptics
import GameController

/// A pad's motors through GCController.haptics: one CoreHaptics engine per place that runs (the left and the right
/// handle, else the pad's default locality), made when it first runs. A run plays a looping continuous event at the
/// step's intensity until the next call; PadRumbleHost times the steps. An engine that iOS stopped (the background, a
/// reset) starts again at the next run.
@MainActor
final class ControllerHaptics: PadHaptics {
    private let haptics: GCDeviceHaptics
    private let layout: PadMotorLayout
    private var engines: [PadMotorLayout.Place: CHHapticEngine] = [:]
    private var players: [CHHapticAdvancedPatternPlayer] = []

    init?(_ controller: GCController) {
        guard let haptics = controller.haptics else { return nil }
        self.haptics = haptics
        let places = haptics.supportedLocalities
        layout = PadMotorLayout(hasLeftHandle: places.contains(.leftHandle),
                                hasRightHandle: places.contains(.rightHandle))
    }

    func run(strong: Float, weak: Float) {
        stop()
        for (place, intensity) in layout.intensities(strong: strong, weak: weak) where intensity > 0 {
            guard let engine = engine(place) else { continue }
            do {
                try engine.start()
                let event = CHHapticEvent(
                    eventType: .hapticContinuous,
                    parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity)],
                    relativeTime: 0, duration: 1)
                let player = try engine.makeAdvancedPlayer(with: CHHapticPattern(events: [event], parameters: []))
                player.loopEnabled = true
                try player.start(atTime: CHHapticTimeImmediate)
                players.append(player)
            } catch {
                #if DEBUG
                SessionCheck.event("pad haptics failed \(place)")
                #endif
            }
        }
    }

    func stop() {
        for player in players { try? player.stop(atTime: CHHapticTimeImmediate) }
        players = []
    }

    func release() {
        stop()
        for engine in engines.values { engine.stop() }
        engines = [:]
    }

    private func engine(_ place: PadMotorLayout.Place) -> CHHapticEngine? {
        if let engine = engines[place] { return engine }
        let locality: GCHapticsLocality = switch place {
        case .default: .default
        case .leftHandle: .leftHandle
        case .rightHandle: .rightHandle
        }
        guard let engine = haptics.createEngine(withLocality: locality) else { return nil }
        engines[place] = engine
        return engine
    }
}

#if DEBUG
/// SessionCheck's pad-rumble: the motors' commands as events, "pad <id> rumble run <strong> <weak>", "... stop" and
/// "... release".
@MainActor
final class LoggingHaptics: PadHaptics {
    private let id: Int32

    init(id: Int32) {
        self.id = id
    }

    func run(strong: Float, weak: Float) {
        SessionCheck.event("pad \(id) rumble run \(strong) \(weak)")
    }

    func stop() {
        SessionCheck.event("pad \(id) rumble stop")
    }

    func release() {
        SessionCheck.event("pad \(id) rumble release")
    }
}
#endif
