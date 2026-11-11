import Testing

@testable import APKSession

/// A pad's rumble as Android 15 plays an input device's vibration: InputManagerService's VibrationInfo turns the
/// effect into steps of a duration and an amplitude (DEFAULT_AMPLITUDE as 192), nativeVibrate gives every step to
/// both of an FF_RUMBLE pad's vibrators (0 the strong motor, 1 the weak), and VibratorInputMapper plays the steps one
/// after another, from the repeat index again when there is one, stops at the end otherwise, and stops on a cancel
/// with the token that started it.
@Suite struct PadRumbleTests {
    private let ms: Int64 = 1_000_000

    private func describe(_ commands: [PadRumble.Command]) -> [String] {
        commands.map {
            switch $0 {
            case .run(let strong, let weak): "run \(strong) \(weak)"
            case .stop: "stop"
            }
        }
    }

    @Test func aOneShotRunsBothMotorsForItsDurationThenStops() throws {
        var rumble = PadRumble()
        let shot = try #require(PadVibration(durations: [300], amplitudes: [255], repeatIndex: -1, token: 7))
        #expect(describe(rumble.vibrate(shot, now: 0)) == ["run 1.0 1.0"])
        #expect(rumble.isVibrating)
        #expect(rumble.nextStepTime == 300 * ms)
        #expect(describe(rumble.advance(now: 299 * ms)).isEmpty)
        #expect(describe(rumble.advance(now: 300 * ms)) == ["stop"])
        #expect(!rumble.isVibrating)
        #expect(rumble.nextStepTime == nil)
    }

    @Test func theDefaultAmplitudeIs192AsInputManagerServiceGivesIt() throws {
        let shot = try #require(PadVibration(durations: [100], amplitudes: [PadVibration.defaultAmplitude],
                                             repeatIndex: -1, token: 1))
        #expect(shot.steps == [PadVibration.Step(milliseconds: 100, strong: 192, weak: 192)])
        var rumble = PadRumble()
        #expect(describe(rumble.vibrate(shot, now: 0)) == ["run \(Float(192) / 255) \(Float(192) / 255)"])
    }

    @Test func aWaveformPlaysItsStepsAndAZeroStepIsAPause() throws {
        var rumble = PadRumble()
        let wave = try #require(PadVibration(durations: [100, 50, 200], amplitudes: [255, 0, 51], repeatIndex: -1,
                                             token: 3))
        #expect(describe(rumble.vibrate(wave, now: 0)) == ["run 1.0 1.0"])
        #expect(describe(rumble.advance(now: 100 * ms)) == ["stop"])
        #expect(rumble.isVibrating)
        #expect(describe(rumble.advance(now: 150 * ms)) == ["run 0.2 0.2"])
        #expect(describe(rumble.advance(now: 350 * ms)) == ["stop"])
        #expect(!rumble.isVibrating)
    }

    @Test func aRepeatingWaveformStartsAgainAtItsRepeatIndexUntilCancelled() throws {
        var rumble = PadRumble()
        let wave = try #require(PadVibration(durations: [100, 100, 100], amplitudes: [255, 0, 128], repeatIndex: 1,
                                             token: 9))
        _ = rumble.vibrate(wave, now: 0)
        _ = rumble.advance(now: 100 * ms)  // step 1: the pause
        _ = rumble.advance(now: 200 * ms)  // step 2: 128
        #expect(describe(rumble.advance(now: 300 * ms)) == ["stop"])  // back to step 1
        #expect(rumble.isVibrating)
        #expect(describe(rumble.advance(now: 400 * ms)) == ["run \(Float(128) / 255) \(Float(128) / 255)"])
        #expect(describe(rumble.cancel(token: 8)).isEmpty)  // another token's cancel does nothing
        #expect(rumble.isVibrating)
        #expect(describe(rumble.cancel(token: 9)) == ["stop"])
        #expect(!rumble.isVibrating)
        #expect(rumble.nextStepTime == nil)
    }

    @Test func twoMotorsTakeTheirOwnAmplitudesFromAStereoEffect() throws {
        let stereo = try #require(PadVibration(durations: [100], strong: [255], weak: [51], repeatIndex: -1,
                                               token: 2))
        var rumble = PadRumble()
        #expect(describe(rumble.vibrate(stereo, now: 0)) == ["run 1.0 0.2"])
    }

    @Test func aNewVibrationReplacesTheOneThatPlays() throws {
        var rumble = PadRumble()
        let long = try #require(PadVibration(durations: [1000], amplitudes: [255], repeatIndex: -1, token: 1))
        let short = try #require(PadVibration(durations: [50], amplitudes: [51], repeatIndex: -1, token: 2))
        _ = rumble.vibrate(long, now: 0)
        #expect(describe(rumble.vibrate(short, now: 10 * ms)) == ["run 0.2 0.2"])
        #expect(rumble.nextStepTime == 60 * ms)
        #expect(describe(rumble.cancel(token: 1)).isEmpty)  // the first token no longer plays
    }

    @Test func aStepIsAtMost100SecondsAndAPatternAtMost100Steps() throws {
        let long = try #require(PadVibration(durations: [500_000], amplitudes: [255], repeatIndex: -1, token: 1))
        #expect(long.steps.first?.milliseconds == 100_000)
        #expect(PadVibration(durations: Array(repeating: 10, count: 101), amplitudes: Array(repeating: 255, count: 101),
                             repeatIndex: -1, token: 1) == nil)
        #expect(PadVibration(durations: [10, 10], amplitudes: [255], repeatIndex: -1, token: 1) == nil)
        #expect(PadVibration(durations: [10], amplitudes: [255], repeatIndex: 1, token: 1) == nil)
    }

    @Test func stopAllEndsAVibrationWhateverItsToken() throws {
        var rumble = PadRumble()
        let shot = try #require(PadVibration(durations: [1000], amplitudes: [255], repeatIndex: 0, token: 4))
        _ = rumble.vibrate(shot, now: 0)
        #expect(describe(rumble.stopAll()) == ["stop"])
        #expect(!rumble.isVibrating)
        #expect(describe(rumble.stopAll()).isEmpty)  // nothing plays: nothing to stop
    }
}
