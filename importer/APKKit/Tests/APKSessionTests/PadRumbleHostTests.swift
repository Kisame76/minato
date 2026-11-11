import Testing

@testable import APKSession

/// The pads' rumble on the host without a pad: a fake haptics provider records what the pads' motors are told. A pad's
/// haptics are made at its first vibration only, go when the pad goes or the session ends, and stop when the app goes
/// to the background; a pad without haptics has no vibrator, as Android 15 lists a pad without FF_RUMBLE.
@MainActor @Suite struct PadRumbleHostTests {
    private let ms: Int64 = 1_000_000

    final class FakeHaptics: PadHaptics {
        var log: [String] = []
        func run(strong: Float, weak: Float) { log.append("run \(strong) \(weak)") }
        func stop() { log.append("stop") }
        func release() { log.append("release") }
    }

    final class Pads {
        var made: [Int32: FakeHaptics] = [:]
        var withHaptics: Set<Int32> = [1010]
    }

    private func host(_ pads: Pads) -> PadRumbleHost {
        PadRumbleHost { id in
            guard pads.withHaptics.contains(id) else { return nil }
            let haptics = FakeHaptics()
            pads.made[id] = haptics
            return haptics
        }
    }

    private func shot(_ ms: Int64, _ amplitude: UInt8 = 255, token: Int32 = 1, repeatIndex: Int = -1) -> PadVibration {
        PadVibration(durations: [ms], amplitudes: [amplitude], repeatIndex: repeatIndex, token: token)!
    }

    @Test func aPadsHapticsAreMadeAtItsFirstVibrationOnly() {
        let pads = Pads()
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        #expect(pads.made.isEmpty)
        #expect(rumble.vibrate(1010, shot(300), now: 0) == 300 * ms)
        #expect(pads.made[1010]?.log == ["run 1.0 1.0"])
        #expect(rumble.isVibrating(1010))
        #expect(rumble.advance(now: 300 * ms) == nil)
        #expect(pads.made[1010]?.log == ["run 1.0 1.0", "stop"])
        #expect(!rumble.isVibrating(1010))
    }

    @Test func onlyAPadWithHapticsHasVibrators() {
        let pads = Pads()
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        rumble.add(1011, hasHaptics: false)
        #expect(rumble.vibratorIDs(1010) == [0, 1])
        #expect(rumble.vibratorIDs(1011) == [])
        #expect(rumble.vibratorIDs(1012) == [])
        #expect(rumble.vibrate(1011, shot(300), now: 0) == nil)
        #expect(pads.made[1011] == nil)
    }

    @Test func theEarliestStepOfAllPadsIsTheNextDeadline() {
        let pads = Pads()
        pads.withHaptics = [1010, 1011]
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        rumble.add(1011, hasHaptics: true)
        _ = rumble.vibrate(1010, shot(300), now: 0)
        #expect(rumble.vibrate(1011, shot(100), now: 0) == 100 * ms)
        #expect(rumble.advance(now: 100 * ms) == 300 * ms)
        #expect(pads.made[1011]?.log == ["run 1.0 1.0", "stop"])
        #expect(pads.made[1010]?.log == ["run 1.0 1.0"])
    }

    @Test func aCancelStopsOnlyTheTokensVibration() {
        let pads = Pads()
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        _ = rumble.vibrate(1010, shot(1000, token: 5), now: 0)
        #expect(rumble.cancel(1010, token: 6) == 1000 * ms)
        #expect(rumble.cancel(1010, token: 5) == nil)
        #expect(pads.made[1010]?.log == ["run 1.0 1.0", "stop"])
    }

    @Test func theBackgroundStopsEveryPadButKeepsItsHaptics() {
        let pads = Pads()
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        _ = rumble.vibrate(1010, shot(100, repeatIndex: 0), now: 0)
        rumble.stopAll()
        #expect(pads.made[1010]?.log == ["run 1.0 1.0", "stop"])
        #expect(!rumble.isVibrating(1010))
        _ = rumble.vibrate(1010, shot(100), now: 0)
        #expect(pads.made.count == 1)  // the same haptics again
    }

    @Test func aPadThatGoesStopsAndReleasesItsHaptics() {
        let pads = Pads()
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        _ = rumble.vibrate(1010, shot(1000), now: 0)
        rumble.remove(1010)
        #expect(pads.made[1010]?.log == ["run 1.0 1.0", "stop", "release"])
        #expect(rumble.vibratorIDs(1010) == [])
        #expect(rumble.advance(now: 1000 * ms) == nil)
    }

    @Test func theSessionsEndReleasesEveryPadsHaptics() {
        let pads = Pads()
        pads.withHaptics = [1010, 1011]
        let rumble = host(pads)
        rumble.add(1010, hasHaptics: true)
        rumble.add(1011, hasHaptics: true)
        _ = rumble.vibrate(1010, shot(1000), now: 0)
        rumble.end()
        #expect(pads.made[1010]?.log == ["run 1.0 1.0", "stop", "release"])
        #expect(pads.made[1011] == nil)  // never vibrated: never made
    }

    @Test func twoHandlesTakeTheStrongAndTheWeakMotorElseOneTakesTheStronger() {
        #expect(PadMotorLayout(hasLeftHandle: true, hasRightHandle: true) == .handles)
        #expect(PadMotorLayout(hasLeftHandle: true, hasRightHandle: false) == .single)
        #expect(PadMotorLayout(hasLeftHandle: false, hasRightHandle: false) == .single)
        #expect(PadMotorLayout.handles.intensities(strong: 1, weak: 0.2) == [.leftHandle: 1, .rightHandle: 0.2])
        #expect(PadMotorLayout.single.intensities(strong: 0.4, weak: 0.6) == [.default: 0.6])
    }
}
