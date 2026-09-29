import Synchronization

/// AAudio's performance modes (`aaudio.h`).
public enum AudioPerformanceMode: Int32, Sendable {
    case none = 10
    case powerSaving = 11
    case lowLatency = 12
}

/// The output hardware as the audio session reports it.
public struct AudioHardware: Equatable, Sendable {
    public var sampleRate: Double
    public var ioBufferDuration: Double

    public init(sampleRate: Double, ioBufferDuration: Double) {
        self.sampleRate = sampleRate
        self.ioBufferDuration = ioBufferDuration
    }

    /// A stream's burst: the IO buffer's frames (ADR-0036, decision 6).
    public var burstFrames: Int { Int((sampleRate * ioBufferDuration).rounded()) }
}

/// The host side of the guest's audio output (ADR-0036, decision 4): what the audio session and the engine
/// do as streams open and close, at interruptions and at route changes. The app carries out the actions.
/// A stream is the runtime's handle (`apkrt.h`).
public struct AudioOutput: Sendable {
    public enum Action: Equatable, Sendable {
        case preferIOBuffer(Double)
        /// The session, category `.ambient`, becomes active.
        case activate
        case deactivate
        case startEngine
        case stopEngine
        /// The running engine's output unit and latency go to the runtime (`apkrt_audio_set_output`).
        case publishOutput
        /// The runtime's output is NULL: the engine stops or an interruption stopped it.
        case withdrawOutput
        /// These streams get DISCONNECTED, and their nodes go.
        case disconnect([UInt64])
    }

    private var streams: [UInt64: AudioPerformanceMode] = [:]
    private var ioBuffer: Double?
    private var hardware: AudioHardware?
    private var interrupted = false

    public init() {}

    /// 5 ms once a stream wants LOW_LATENCY, 20 ms otherwise.
    public static func preferredIOBufferDuration(for modes: some Sequence<AudioPerformanceMode>) -> Double {
        modes.contains(.lowLatency) ? 0.005 : 0.020
    }

    public mutating func opened(_ stream: UInt64, mode: AudioPerformanceMode) -> [Action] {
        let first = streams.isEmpty
        streams[stream] = mode
        var actions = preferIOBuffer()
        if first && !interrupted { actions += [.activate, .startEngine, .publishOutput] }
        return actions
    }

    public mutating func closed(_ stream: UInt64) -> [Action] {
        guard streams.removeValue(forKey: stream) != nil else { return [] }
        return streams.isEmpty ? stopped() : []
    }

    /// A call or Siri: iOS stopped the engine.
    public mutating func interruptionBegan() -> [Action] {
        interrupted = true
        return streams.isEmpty ? [] : [.withdrawOutput]
    }

    /// The streams stay STARTED, so the engine starts again (ADR-0036, decision 6).
    public mutating func interruptionEnded() -> [Action] {
        interrupted = false
        return streams.isEmpty ? [] : [.activate, .startEngine, .publishOutput]
    }

    /// Back from the background, where iOS stopped the engine: `.ambient` has no background audio.
    public mutating func enteredForeground() -> [Action] {
        streams.isEmpty || interrupted ? [] : [.activate, .startEngine, .publishOutput]
    }

    /// The engine stopped itself at a change of its configuration.
    public mutating func engineStopped() -> [Action] {
        streams.isEmpty || interrupted ? [] : [.startEngine, .publishOutput]
    }

    /// The hardware now, after the host's own change of the IO buffer or, `rerouted`, after a route change.
    /// A reroute that changes the rate or the burst disconnects every stream (AudioStreamTrack.cpp); any other
    /// goes on, on an output whose latency may have changed.
    public mutating func hardware(_ now: AudioHardware, rerouted: Bool) -> [Action] {
        defer { hardware = now }
        guard rerouted, !streams.isEmpty else { return [] }
        // During an interruption the engine is stopped; its end publishes the output.
        guard let before = hardware, before.sampleRate != now.sampleRate || before.burstFrames != now.burstFrames
        else { return interrupted ? [] : [.publishOutput] }
        let disconnected = streams.keys.sorted()
        streams = [:]
        return [.disconnect(disconnected)] + stopped()
    }

    /// A stream's burst is fixed at its open (`apkrt.h`), so the IO buffer shrinks for a LOW_LATENCY stream but
    /// never grows while streams are open: the 5 ms stay until the last one closes.
    private mutating func preferIOBuffer() -> [Action] {
        let duration = Self.preferredIOBufferDuration(for: streams.values)
        guard ioBuffer.map({ duration < $0 }) ?? true else { return [] }
        ioBuffer = duration
        return [.preferIOBuffer(duration)]
    }

    private mutating func stopped() -> [Action] {
        ioBuffer = nil
        return [.withdrawOutput, .stopEngine, .deactivate]
    }
}

/// The render block's source: a stream's ring (ADR-0036, decision 5), which Runtime fills from the guest.
/// `read` runs on the real-time audio thread: no lock, no allocation, no guest code.
public protocol AudioRenderSource: AnyObject, Sendable {
    /// Copies up to `frames` frames of `channels` interleaved floats into `buffer` and returns how many;
    /// `hostTime` is the output's time for the first (`AudioTimeStamp.mHostTime`, the guest's clock).
    func read(into buffer: UnsafeMutablePointer<Float>, frames: Int, channels: Int, hostTime: UInt64) -> Int
}

/// One output stream as its render block sees it.
public final class AudioOutputStream: Sendable {
    public let channels: Int
    public let source: any AudioRenderSource
    private let xrunCount = Atomic<Int>(0)
    private let played = Atomic<Bool>(false)

    public init(channels: Int, source: any AudioRenderSource) {
        self.channels = channels
        self.source = source
    }

    /// Gaps after the stream's first frames.
    public var xruns: Int { xrunCount.load(ordering: .relaxed) }

    /// The render block's work, real-time safe: `frames` frames from the ring, zeros for a gap, and the gap
    /// counted as an xrun once the stream has played. Returns the frames the ring had.
    public func render(into buffer: UnsafeMutablePointer<Float>, frames: Int, hostTime: UInt64) -> Int {
        let read = min(max(source.read(into: buffer, frames: frames, channels: channels, hostTime: hostTime), 0), frames)
        if read < frames {
            (buffer + read * channels).update(repeating: 0, count: (frames - read) * channels)
            if read > 0 || played.load(ordering: .relaxed) { xrunCount.add(1, ordering: .relaxed) }
        }
        if read > 0 { played.store(true, ordering: .relaxed) }
        return read
    }
}
