import APKSession
import AVFAudio
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "audio")

/// Where the host's news about the guest's streams goes: Runtime's `libaaudio` (ADR-0036, decision 6).
protocol AudioStreamEventSink: AnyObject, Sendable {
    /// A reroute changed the hardware's rate or burst: these streams are DISCONNECTED. On the host's queue.
    func disconnected(_ streams: [Int])
}

/// The audio host side (ADR-0036, decision 4): the audio session, category `.ambient`, and one engine with
/// one source node per output stream into its main mixer, which converts rate and channels. `AudioOutput`
/// decides; this carries it out, and follows interruptions, route changes, the engine's own stops and the
/// return from the background.
///
/// Everything runs on a serial queue of its own, never the main thread: `libaaudio`'s calls come from guest
/// threads, and the main thread can be busy with SwiftUI or wait for "scheduled" in the background.
final class AudioHost: @unchecked Sendable {
    /// All state below is only touched on this queue.
    private let queue = DispatchQueue(label: "Minato audio host", qos: .userInitiated)
    private let session = AVAudioSession.sharedInstance()
    private let engine = AVAudioEngine()
    private let sink: AudioStreamEventSink
    private var output = AudioOutput()
    private var nodes: [Int: AVAudioSourceNode] = [:]
    private var nextStream = 1
    private var observers: [any NSObjectProtocol] = []

    init(sink: AudioStreamEventSink) {
        self.sink = sink
        // The mixer and its connection to the output exist before the engine first starts.
        _ = engine.mainMixerNode
        let center = NotificationCenter.default
        let onQueue = OperationQueue()
        onQueue.underlyingQueue = queue
        onQueue.maxConcurrentOperationCount = 1
        observers = [
            center.addObserver(forName: AVAudioSession.interruptionNotification, object: session, queue: onQueue) {
                [weak self] note in
                let began = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt)
                    .flatMap(AVAudioSession.InterruptionType.init) == .began
                self?.interrupted(began: began)
            },
            center.addObserver(forName: AVAudioSession.routeChangeNotification, object: session, queue: onQueue) {
                [weak self] note in
                let reason = (note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt)
                    .flatMap(AVAudioSession.RouteChangeReason.init)
                self?.rerouted(reason)
            },
            center.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: onQueue) {
                [weak self] _ in
                self?.engineStopped()
            },
            center.addObserver(forName: UIScene.willEnterForegroundNotification, object: nil, queue: onQueue) {
                [weak self] _ in
                self?.enteredForeground()
            },
        ]
    }

    /// The hardware now.
    private var hardware: AudioHardware {
        AudioHardware(sampleRate: session.sampleRate, ioBufferDuration: session.ioBufferDuration)
    }

    /// Opens an output stream of `channels` at `sampleRate`, 0 for the hardware's, reading `source`.
    /// Returns its ID, what its render block counts, and the hardware it starts on; nil when the format fails.
    /// From any thread but the host's queue.
    func open(sampleRate: Double, channels: Int, mode: AudioPerformanceMode, source: any AudioRenderSource)
        -> (id: Int, stream: AudioOutputStream, hardware: AudioHardware)? {
        dispatchPrecondition(condition: .notOnQueue(queue))
        return queue.sync { opened(sampleRate: sampleRate, channels: channels, mode: mode, source: source) }
    }

    /// From any thread but the host's queue.
    func close(_ id: Int) {
        dispatchPrecondition(condition: .notOnQueue(queue))
        queue.sync {
            detach([id])
            apply(output.closed(id))
        }
    }

    private func opened(sampleRate: Double, channels: Int, mode: AudioPerformanceMode, source: any AudioRenderSource)
        -> (id: Int, stream: AudioOutputStream, hardware: AudioHardware)? {
        let id = nextStream
        nextStream += 1
        apply(output.opened(id, mode: mode))
        let now = hardware
        let rate = sampleRate > 0 ? sampleRate : now.sampleRate
        // The ring's frames are interleaved, as AAudio's; the mixer takes only the standard, deinterleaved
        // format, to which the node converts. The mixer converts the rate.
        guard let ring = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate,
                                       channels: AVAudioChannelCount(channels), interleaved: true),
              let connection = AVAudioFormat(standardFormatWithSampleRate: rate, channels: AVAudioChannelCount(channels))
        else {
            apply(output.closed(id))
            return nil
        }
        let stream = AudioOutputStream(channels: channels, source: source)
        let node = AVAudioSourceNode(format: ring, renderBlock: Self.renderBlock(stream))
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: connection)
        nodes[id] = node
        return (id, stream, now)
    }

    /// The render block, on the real-time thread: it reads the ring through `stream`, and nothing else
    /// runs there; `UnsafeMutableAudioBufferListPointer` is a view without allocation.
    private static func renderBlock(_ stream: AudioOutputStream) -> AVAudioSourceNodeRenderBlock {
        { isSilence, timestamp, frameCount, bufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard let data = buffers.first?.mData else { return noErr }
            let read = stream.render(into: data.assumingMemoryBound(to: Float.self), frames: Int(frameCount),
                                     hostTime: timestamp.pointee.mHostTime)
            isSilence.pointee = ObjCBool(read == 0)
            return noErr
        }
    }

    private func interrupted(began: Bool) {
        #if DEBUG
        SessionCheck.event(began ? "audio interruption began" : "audio interruption ended")
        #endif
        apply(began ? output.interruptionBegan() : output.interruptionEnded())
    }

    /// The session's own change of category is no reroute.
    private func rerouted(_ reason: AVAudioSession.RouteChangeReason?) {
        #if DEBUG
        SessionCheck.event("audio route changed \(reason.map { String($0.rawValue) } ?? "unknown")")
        #endif
        let device: Set<AVAudioSession.RouteChangeReason> = [
            .newDeviceAvailable, .oldDeviceUnavailable, .override, .wakeFromSleep, .noSuitableRouteForCategory,
            .routeConfigurationChange,
        ]
        apply(output.hardware(hardware, rerouted: reason.map(device.contains) ?? false))
    }

    private func enteredForeground() {
        #if DEBUG
        SessionCheck.event("audio foreground engine running \(engine.isRunning)")
        #endif
        apply(output.enteredForeground())
    }

    private func engineStopped() {
        guard !engine.isRunning else { return }
        apply(output.engineStopped())
    }

    private func detach(_ streams: [Int]) {
        for id in streams {
            guard let node = nodes.removeValue(forKey: id) else { continue }
            engine.detach(node)
        }
    }

    private func apply(_ actions: [AudioOutput.Action]) {
        for action in actions {
            do {
                switch action {
                case .preferIOBuffer(let duration):
                    try session.setPreferredIOBufferDuration(duration)
                case .activate:
                    try session.setCategory(.ambient)
                    try session.setActive(true)
                case .deactivate:
                    try session.setActive(false)
                case .startEngine:
                    try engine.start()
                case .stopEngine:
                    engine.stop()
                case .disconnect(let streams):
                    detach(streams)
                    sink.disconnected(streams)
                }
                #if DEBUG
                SessionCheck.audio(action, hardware: hardware)
                #endif
            } catch {
                log.error("audio \(String(describing: action), privacy: .public) failed: \(error, privacy: .public)")
                #if DEBUG
                SessionCheck.event("audio \(action) failed \(error)")
                #endif
            }
        }
        // The IO buffer the session gave, which is no reroute.
        if actions.contains(where: { if case .preferIOBuffer = $0 { true } else { $0 == .activate } }) {
            _ = output.hardware(hardware, rerouted: false)
        }
    }
}

/// The host's news until Runtime's `libaaudio` exists (ADR-0036, step c): the Debug build logs it for
/// session-check, and nothing goes further.
final class NoChannelAudioSink: AudioStreamEventSink {
    func disconnected(_ streams: [Int]) {
        #if DEBUG
        SessionCheck.event("audio disconnected \(streams.map(String.init).joined(separator: ","))")
        #endif
    }
}
