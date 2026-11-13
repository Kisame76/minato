import Synchronization
import Testing

@testable import APKSession

/// A stream's ring as the tests fill it: interleaved floats, read on the render thread.
final class TestRing: AudioRenderSource {
    private let samples = Mutex<[Float]>([])
    let hostTimes = Mutex<[UInt64]>([])

    func fill(_ values: [Float]) {
        samples.withLock { $0 += values }
    }

    func read(into buffer: UnsafeMutablePointer<Float>, frames: Int, channels: Int, hostTime: UInt64) -> Int {
        hostTimes.withLock { $0.append(hostTime) }
        return samples.withLock { samples in
            let count = min(frames, samples.count / channels)
            for index in 0..<count * channels { buffer[index] = samples[index] }
            samples.removeFirst(count * channels)
            return count
        }
    }
}

/// ADR-0036, decisions 4 and 5: the audio session's host side, and the render block's reads.
@Suite("AudioOutput")
struct AudioOutputTests {
    static func render(_ stream: AudioOutputStream, frames: Int, hostTime: UInt64 = 0) -> ([Float], Int) {
        var buffer = [Float](repeating: .nan, count: frames * stream.channels)
        let read = buffer.withUnsafeMutableBufferPointer { stream.render(into: $0.baseAddress!, frames: frames, hostTime: hostTime) }
        return (buffer, read)
    }

    // MARK: The IO buffer and the hardware

    @Test func theIOBufferIs5MsOnceAStreamWantsLowLatencyAnd20MsOtherwise() {
        #expect(AudioOutput.preferredIOBufferDuration(for: []) == 0.020)
        #expect(AudioOutput.preferredIOBufferDuration(for: [.none, .powerSaving]) == 0.020)
        #expect(AudioOutput.preferredIOBufferDuration(for: [.none, .lowLatency]) == 0.005)
    }

    @Test func performanceModesHaveAAudiosValues() {
        #expect(AudioPerformanceMode(rawValue: 10) == AudioPerformanceMode.none)
        #expect(AudioPerformanceMode(rawValue: 11) == .powerSaving)
        #expect(AudioPerformanceMode(rawValue: 12) == .lowLatency)
    }

    /// A burst is the IO buffer's frames (decision 6).
    @Test func aBurstIsTheIOBuffersFrames() {
        #expect(AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.005).burstFrames == 240)
        #expect(AudioHardware(sampleRate: 44_100, ioBufferDuration: 0.02).burstFrames == 882)
        #expect(AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.0053333).burstFrames == 256)
    }

    // MARK: The session and the engine

    @Test func theFirstStreamActivatesTheSessionAndStartsTheEngine() {
        var output = AudioOutput()

        #expect(output.opened(1, mode: .none) == [.preferIOBuffer(0.020), .activate, .startEngine, .publishOutput])
    }

    /// A stream's burst is fixed at its open (by the runtime), so the IO buffer shrinks for a LOW_LATENCY stream
    /// but never grows while streams are open: a burst would outgrow the rings of the streams opened on it.
    @Test func aLowLatencyStreamShortensTheIOBufferUntilTheLastStreamCloses() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)

        #expect(output.opened(2, mode: .lowLatency) == [.preferIOBuffer(0.005)])
        #expect(output.opened(3, mode: .powerSaving) == [])
        #expect(output.closed(2) == [])
        #expect(output.opened(4, mode: .none) == [])
        _ = output.closed(1)
        _ = output.closed(3)
        #expect(output.closed(4) == [.withdrawOutput, .stopEngine, .deactivate])
        #expect(output.opened(5, mode: .none) == [.preferIOBuffer(0.020), .activate, .startEngine, .publishOutput])
    }

    @Test func theLastStreamStopsTheEngineAndDeactivatesTheSession() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.opened(2, mode: .none)

        #expect(output.closed(1) == [])
        #expect(output.closed(2) == [.withdrawOutput, .stopEngine, .deactivate])
        #expect(output.closed(2) == [])
    }

    /// A call or Siri stops the engine; the streams stay STARTED and play again after it (decision 6).
    @Test func afterAnInterruptionTheEngineStartsAgain() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)

        #expect(output.interruptionBegan() == [.withdrawOutput])
        #expect(output.interruptionEnded() == [.activate, .startEngine, .publishOutput])
    }

    @Test func aStreamOpenedDuringAnInterruptionWaitsForItsEnd() {
        var output = AudioOutput()
        _ = output.interruptionBegan()

        #expect(output.opened(1, mode: .lowLatency) == [.preferIOBuffer(0.005)])
        #expect(output.interruptionEnded() == [.activate, .startEngine, .publishOutput])
    }

    /// Streams are the runtime's handles (`apkrt.h`), which use all 64 bits.
    @Test func streamsAreTheRuntimesHandles() {
        var output = AudioOutput()
        let handle: UInt64 = 0xFFFF_FFFF_FFFF_FFC1
        _ = output.opened(handle, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.hardware(AudioHardware(sampleRate: 44_100, ioBufferDuration: 0.005), rerouted: true)
            == [.disconnect([handle]), .withdrawOutput, .stopEngine, .deactivate])
    }

    @Test func anInterruptionWithoutStreamsEndsWithNothingToDo() {
        var output = AudioOutput()

        #expect(output.interruptionBegan() == [])
        #expect(output.interruptionEnded() == [])
    }

    /// With `.ambient` and no background audio, iOS stops the engine in the background.
    @Test func atTheForegroundTheEngineStartsAgain() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)

        #expect(output.enteredForeground() == [.activate, .startEngine, .publishOutput])
    }

    @Test func theForegroundWithoutStreamsOrDuringAnInterruptionStartsNothing() {
        var output = AudioOutput()
        #expect(output.enteredForeground() == [])

        _ = output.opened(1, mode: .none)
        _ = output.interruptionBegan()
        #expect(output.enteredForeground() == [])
        #expect(output.interruptionEnded() == [.activate, .startEngine, .publishOutput])
    }

    /// AVAudioEngineConfigurationChange: the engine stopped itself.
    @Test func anEngineThatStoppedItselfStartsAgain() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)

        #expect(output.engineStopped() == [.startEngine, .publishOutput])
    }

    @Test func anEngineThatStoppedDuringAnInterruptionWaitsForItsEnd() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.interruptionBegan()

        #expect(output.engineStopped() == [])
    }

    @Test func anEngineWithoutStreamsStaysStopped() {
        var output = AudioOutput()

        #expect(output.engineStopped() == [])
    }

    // MARK: Routes

    static let speaker = AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.005)

    /// The streams go on, on an output whose latency may have changed.
    @Test func aRouteChangeKeepsTheStreamsWhenRateAndBurstStay() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.hardware(AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.0050001), rerouted: true)
            == [.publishOutput])
    }

    /// The interruption's end publishes the output.
    @Test func aRouteChangeDuringAnInterruptionPublishesNothing() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)
        _ = output.interruptionBegan()

        #expect(output.hardware(Self.speaker, rerouted: true) == [])
    }

    /// Headphones out during a call: the streams' burst or rate is gone all the same.
    @Test func aRouteChangeOfRateDuringAnInterruptionStillDisconnects() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)
        _ = output.interruptionBegan()

        #expect(output.hardware(AudioHardware(sampleRate: 44_100, ioBufferDuration: 0.005), rerouted: true)
            == [.disconnect([1]), .withdrawOutput, .stopEngine, .deactivate])
        #expect(output.interruptionEnded() == [])
    }

    /// AudioStreamTrack.cpp: a reroute that changes the rate or the burst disconnects.
    @Test func aRouteChangeOfRateDisconnectsEveryStream() {
        var output = AudioOutput()
        _ = output.opened(2, mode: .none)
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.hardware(AudioHardware(sampleRate: 44_100, ioBufferDuration: 0.005), rerouted: true)
            == [.disconnect([1, 2]), .withdrawOutput, .stopEngine, .deactivate])
        #expect(output.closed(1) == [])
    }

    @Test func aRouteChangeOfBurstDisconnects() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.hardware(AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.02), rerouted: true)
            == [.disconnect([1]), .withdrawOutput, .stopEngine, .deactivate])
    }

    /// The host's own change of the IO buffer is no reroute.
    @Test func theHostsOwnIOBufferChangeKeepsTheStreams() {
        var output = AudioOutput()
        _ = output.opened(1, mode: .none)
        _ = output.hardware(AudioHardware(sampleRate: 48_000, ioBufferDuration: 0.02), rerouted: false)
        _ = output.opened(2, mode: .lowLatency)

        #expect(output.hardware(Self.speaker, rerouted: false) == [])
        #expect(output.hardware(Self.speaker, rerouted: true) == [.publishOutput])
    }

    /// session-check's `audio-disconnect` (audio-live): what a reroute that changes the rate does, without one.
    @Test func aForcedDisconnectIsTheReroutesDisconnect() {
        var output = AudioOutput()
        _ = output.opened(2, mode: .lowLatency)
        _ = output.opened(1, mode: .none)
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.disconnectAll() == [.disconnect([1, 2]), .withdrawOutput, .stopEngine, .deactivate])
        #expect(output.disconnectAll() == [])
        #expect(output.opened(3, mode: .lowLatency) == [.preferIOBuffer(0.005), .activate, .startEngine, .publishOutput])
    }

    @Test func aRouteChangeWithoutStreamsDoesNothing() {
        var output = AudioOutput()
        _ = output.hardware(Self.speaker, rerouted: false)

        #expect(output.hardware(AudioHardware(sampleRate: 44_100, ioBufferDuration: 0.005), rerouted: true) == [])
    }

    // MARK: The render block's reads

    @Test func theRenderBlockReadsTheRing() {
        let ring = TestRing()
        let stream = AudioOutputStream(channels: 2, source: ring)
        ring.fill([1, 2, 3, 4, 5, 6])

        let (buffer, read) = Self.render(stream, frames: 3, hostTime: 42)

        #expect(buffer == [1, 2, 3, 4, 5, 6])
        #expect(read == 3)
        #expect(ring.hostTimes.withLock { $0 } == [42])
        #expect(stream.xruns == 0)
    }

    @Test func aGapIsFilledWithZerosAndCountsAsAnXrun() {
        let ring = TestRing()
        let stream = AudioOutputStream(channels: 2, source: ring)
        ring.fill([1, 2])

        let (buffer, read) = Self.render(stream, frames: 3)

        #expect(buffer == [1, 2, 0, 0, 0, 0])
        #expect(read == 1)
        #expect(stream.xruns == 1)
        #expect(Self.render(stream, frames: 2).0 == [0, 0, 0, 0])
        #expect(stream.xruns == 2)
    }

    /// Before the guest's first frames the stream has not played yet: silence, but no xrun.
    @Test func silenceBeforeTheFirstFramesIsNoXrun() {
        let ring = TestRing()
        let stream = AudioOutputStream(channels: 1, source: ring)

        #expect(Self.render(stream, frames: 2) == ([0, 0], 0))
        #expect(stream.xruns == 0)
        ring.fill([1])
        #expect(Self.render(stream, frames: 2) == ([1, 0], 1))
        #expect(stream.xruns == 1)
    }
}
