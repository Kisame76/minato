import Testing

@testable import APKSession

/// ADR-0064 decisions 3 and 6: the guest's input streams on the same audio session and engine as its output. The
/// session is playAndRecord while an input stream is open, the engine's input node has one tap for all of them, and
/// the engine runs while any stream, input or output, is open.
@Suite struct AudioInputTests {
    @Test func theFirstInputRecordsActivatesTapsAndStartsTheEngine() {
        var audio = AudioOutput()
        #expect(audio.inputOpened(64) == [.record(true), .activate, .installTap, .startEngine, .publishOutput])
        #expect(audio.inputOpened(65).isEmpty)
    }

    @Test func anInputWhileTheOutputPlaysRestartsTheEngineInTheRecordingSession() {
        var audio = AudioOutput()
        _ = audio.opened(64, mode: .none)
        #expect(audio.inputOpened(65) == [.withdrawOutput, .stopEngine, .record(true), .activate, .installTap,
                                          .startEngine, .publishOutput])
    }

    @Test func theLastInputGoesAndTheOutputGoesOnWithoutRecording() {
        var audio = AudioOutput()
        _ = audio.opened(64, mode: .none)
        _ = audio.inputOpened(65)
        _ = audio.inputOpened(66)
        #expect(audio.inputClosed(65).isEmpty)
        #expect(audio.inputClosed(66) == [.removeTap, .withdrawOutput, .stopEngine, .record(false), .activate,
                                          .startEngine, .publishOutput])
    }

    @Test func theLastInputWithoutAnOutputStopsEverything() {
        var audio = AudioOutput()
        _ = audio.inputOpened(65)
        #expect(audio.inputClosed(65) == [.removeTap, .withdrawOutput, .stopEngine, .deactivate, .record(false)])
        #expect(audio.inputClosed(65).isEmpty)
    }

    @Test func anOutputBesideAnInputNeitherStartsNorStopsTheEngine() {
        var audio = AudioOutput()
        _ = audio.inputOpened(65)
        #expect(audio.opened(64, mode: .none) == [.preferIOBuffer(0.020)])
        #expect(audio.closed(64).isEmpty)
    }

    @Test func anInterruptionStopsTheCaptureAndItsEndStartsItAgain() {
        var audio = AudioOutput()
        _ = audio.inputOpened(65)
        #expect(audio.interruptionBegan() == [.withdrawOutput])
        #expect(audio.interruptionEnded() == [.activate, .installTap, .startEngine, .publishOutput])
        _ = audio.interruptionBegan()
        #expect(audio.enteredForeground().isEmpty)
    }

    @Test func anInputDuringAnInterruptionWaitsForItsEnd() {
        var audio = AudioOutput()
        _ = audio.interruptionBegan()
        #expect(audio.inputOpened(65) == [.record(true)])
        #expect(audio.interruptionEnded() == [.activate, .installTap, .startEngine, .publishOutput])
    }

    @Test func theForegroundStartsTheCaptureAgain() {
        var audio = AudioOutput()
        _ = audio.inputOpened(65)
        #expect(audio.enteredForeground() == [.activate, .installTap, .startEngine, .publishOutput])
    }

    @Test func anEngineThatStoppedItselfGetsItsTapAgainForTheNewInputFormat() {
        var audio = AudioOutput()
        _ = audio.inputOpened(65)
        #expect(audio.engineStopped() == [.installTap, .startEngine, .publishOutput])
    }

    @Test func aReroutingThatDisconnectsTheOutputsKeepsTheCaptureRunning() {
        var audio = AudioOutput()
        _ = audio.hardware(AudioHardware(sampleRate: 48000, ioBufferDuration: 0.02), rerouted: false)
        _ = audio.opened(64, mode: .none)
        _ = audio.inputOpened(65)
        let actions = audio.hardware(AudioHardware(sampleRate: 44100, ioBufferDuration: 0.02), rerouted: true)
        #expect(actions == [.disconnect([64])])
    }
}
