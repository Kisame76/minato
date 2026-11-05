import APKRuntime
import APKSession
import AVFAudio
import Synchronization
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "audio")

/// Where the host's news about the guest's streams goes: Runtime's `libaaudio` (ADR-0036, decision 6).
protocol AudioStreamEventSink: AnyObject, Sendable {
    /// A reroute changed the hardware's rate or burst: these streams are DISCONNECTED. On the host's queue.
    func disconnected(_ streams: [UInt64])
}

/// The audio host side (ADR-0036, decision 4): the audio session, category `.ambient`, and one engine with
/// one source node per output stream into its main mixer, which converts rate and channels. `AudioOutput`
/// decides; this carries it out, and follows interruptions, route changes, the engine's own stops and the
/// return from the background. The guest's input streams (ADR-0064) make the session playAndRecord while one is open;
/// the input node's one tap converts each buffer to every input stream's rate and channels (MicrophoneTap) and hands
/// it to the runtime's ring with apkrt_audio_input_write.
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
    private var nodes: [UInt64: AVAudioSourceNode] = [:]
    /// The latency last given to the runtime, which a withdrawal keeps.
    private var outputLatency: Int64 = 0
    private var servesRuntime = false
    private var observers: [any NSObjectProtocol] = []
    /// Whether the session records: playAndRecord while an input stream is open.
    private var recording = false
    /// The input streams, which the tap's thread reads too, and whether the input node has the tap.
    private let microphone = MicrophoneTap()
    private var tapped = false

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

    /// Makes this host the runtime's audio side (`apkrt_set_audio_host`), for the guest's `libaaudio`. The
    /// process runs one guest (ADR-0032), so the host stays the runtime's, retained, until the process ends.
    /// From any thread but the host's queue.
    func serveRuntime() {
        dispatchPrecondition(condition: .notOnQueue(queue))
        queue.sync {
            guard !servesRuntime else { return }
            servesRuntime = true
            // The runtime keeps a copy of the record.
            var host = apkrt_audio_host(context: Unmanaged.passRetained(self).toOpaque(),
                                        open: Self.openFromRuntime, close: Self.closeFromRuntime)
            if apkrt_set_audio_host(&host) != 0 { log.error("the runtime took no audio host") }
            var input = apkrt_audio_input_host(context: Unmanaged.passRetained(self).toOpaque(),
                                               open: Self.openInputFromRuntime, close: Self.closeInputFromRuntime)
            if apkrt_set_audio_input_host(&input) != 0 { log.error("the runtime took no audio input host") }
        }
    }

    /// `apkrt_audio_input_host.open`, on the guest thread that opens input `stream`, inside its syscall. The runtime
    /// calls it only with the app's grant; without iOS's own (Minato's) the open fails.
    private static let openInputFromRuntime: @convention(c) (UnsafeMutableRawPointer?, UInt64, Double, Int32, Int32,
                                                             UnsafeMutablePointer<apkrt_audio_hardware>?) -> Int32 = {
        context, stream, sampleRate, channels, _, hardware in
        guard let context, let hardware, channels > 0,
              AVAudioApplication.shared.recordPermission == .granted else { return -1 }
        let host = Unmanaged<AudioHost>.fromOpaque(context).takeUnretainedValue()
        guard let now = host.openInput(stream, sampleRate: sampleRate, channels: Int(channels)) else { return -1 }
        hardware.pointee = apkrt_audio_hardware(sample_rate: now.sampleRate, io_buffer_duration: now.ioBufferDuration)
        return 0
    }

    /// `apkrt_audio_input_host.close`: the guest's close, or the grant's revoke, which closes every input stream.
    private static let closeInputFromRuntime: @convention(c) (UnsafeMutableRawPointer?, UInt64) -> Void = {
        context, stream in
        guard let context else { return }
        #if DEBUG
        SessionCheck.event("audio input closed xruns \(apkrt_audio_xruns(stream))")
        #endif
        Unmanaged<AudioHost>.fromOpaque(context).takeUnretainedValue().closeInput(stream)
    }

    /// Opens input `stream` of `channels` at `sampleRate`, 0 for the hardware's; the hardware it captures on, nil when
    /// the format fails or the stream is open already. From any thread but the host's queue.
    func openInput(_ stream: UInt64, sampleRate: Double, channels: Int) -> AudioHardware? {
        dispatchPrecondition(condition: .notOnQueue(queue))
        return queue.sync {
            guard !microphone.has(stream) else { return nil }
            apply(output.inputOpened(stream))
            let now = hardware
            guard recording, let format = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                                        sampleRate: sampleRate > 0 ? sampleRate : now.sampleRate,
                                                        channels: AVAudioChannelCount(channels), interleaved: true)
            else {
                apply(output.inputClosed(stream))
                return nil
            }
            microphone.add(stream, format: format)
            #if DEBUG
            SessionCheck.event("audio input opened \(Int(format.sampleRate)) Hz \(channels) channels")
            #endif
            return now
        }
    }

    /// From any thread but the host's queue.
    func closeInput(_ stream: UInt64) {
        dispatchPrecondition(condition: .notOnQueue(queue))
        queue.sync {
            guard microphone.remove(stream) else { return }
            apply(output.inputClosed(stream))
        }
    }

    /// `apkrt_audio_host.open`, on the guest thread that opens `stream`, inside its syscall.
    private static let openFromRuntime: @convention(c) (UnsafeMutableRawPointer?, UInt64, Double, Int32, Int32,
                                                        UnsafeMutablePointer<apkrt_audio_hardware>?) -> Int32 = {
        context, stream, sampleRate, channels, mode, hardware in
        guard let context, let hardware else { return -1 }
        let host = Unmanaged<AudioHost>.fromOpaque(context).takeUnretainedValue()
        guard let (_, now) = host.open(stream: stream, sampleRate: sampleRate, channels: Int(channels),
                                       mode: AudioPerformanceMode(rawValue: mode) ?? .none,
                                       source: RuntimeAudioSource(stream: stream))
        else { return -1 }
        hardware.pointee = apkrt_audio_hardware(sample_rate: now.sampleRate, io_buffer_duration: now.ioBufferDuration)
        return 0
    }

    /// `apkrt_audio_host.close`, on the guest thread that closes `stream`, inside its syscall.
    private static let closeFromRuntime: @convention(c) (UnsafeMutableRawPointer?, UInt64) -> Void = { context, stream in
        guard let context else { return }
        #if DEBUG
        SessionCheck.event("audio stream closed xruns \(apkrt_audio_xruns(stream))")
        #endif
        Unmanaged<AudioHost>.fromOpaque(context).takeUnretainedValue().close(stream)
    }

    /// Opens output stream `stream` of `channels` at `sampleRate`, 0 for the hardware's, reading `source`.
    /// Returns what its render block counts, and the hardware it starts on; nil when the format fails or the
    /// stream is open already. From any thread but the host's queue.
    func open(stream: UInt64, sampleRate: Double, channels: Int, mode: AudioPerformanceMode,
              source: any AudioRenderSource) -> (stream: AudioOutputStream, hardware: AudioHardware)? {
        dispatchPrecondition(condition: .notOnQueue(queue))
        return queue.sync { opened(stream, sampleRate: sampleRate, channels: channels, mode: mode, source: source) }
    }

    /// From any thread but the host's queue.
    func close(_ stream: UInt64) {
        dispatchPrecondition(condition: .notOnQueue(queue))
        queue.sync {
            detach([stream])
            apply(output.closed(stream))
        }
    }

    private func opened(_ id: UInt64, sampleRate: Double, channels: Int, mode: AudioPerformanceMode,
                        source: any AudioRenderSource) -> (stream: AudioOutputStream, hardware: AudioHardware)? {
        guard nodes[id] == nil else { return nil }
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
        return (stream, now)
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

    #if DEBUG
    /// session-check's `audio-disconnect` (audio-live): every open stream gets DISCONNECTED, as at a reroute that
    /// changes the rate. From any thread.
    func disconnectAll() {
        queue.async { [self] in apply(output.disconnectAll()) }
    }
    #endif

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

    /// The category the streams call for: playAndRecord on the speaker, as `.ambient` plays there, while recording.
    private func setCategory() throws {
        if recording {
            try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetoothA2DP])
        } else {
            try session.setCategory(.ambient)
        }
    }

    private func detach(_ streams: [UInt64]) {
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
                case .record(let on):
                    recording = on
                    try setCategory()
                case .activate:
                    try setCategory()
                    try session.setActive(true)
                case .installTap:
                    let input = engine.inputNode
                    if tapped { input.removeTap(onBus: 0) }
                    tapped = false
                    let format = input.outputFormat(forBus: 0)
                    // No input in the session's route: the streams read nothing.
                    guard format.sampleRate > 0, format.channelCount > 0 else { break }
                    let microphone = microphone
                    input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, time in
                        microphone.captured(buffer, hostTime: time.isHostTimeValid ? time.hostTime : 0)
                    }
                    tapped = true
                case .removeTap:
                    if tapped { engine.inputNode.removeTap(onBus: 0) }
                    tapped = false
                case .deactivate:
                    try session.setActive(false)
                case .startEngine:
                    try engine.start()
                case .stopEngine:
                    engine.stop()
                case .publishOutput:
                    // An engine that failed to start leaves the output withdrawn. The latency is from a render's
                    // host time to the speaker [guess: the session's output latency is all of it].
                    if engine.isRunning, let unit = engine.outputNode.audioUnit {
                        outputLatency = Int64((session.outputLatency * 1e9).rounded())
                        apkrt_audio_set_output(UnsafeMutableRawPointer(unit), outputLatency)
                    }
                case .withdrawOutput:
                    apkrt_audio_set_output(nil, outputLatency)
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

/// The host's news goes to Runtime's `libaaudio`, whose error callbacks follow; the Debug build also logs it
/// for session-check. session-check's own streams are no runtime handles, which the runtime ignores.
final class RuntimeAudioSink: AudioStreamEventSink {
    func disconnected(_ streams: [UInt64]) {
        for stream in streams { apkrt_audio_disconnected(stream) }
        #if DEBUG
        SessionCheck.event("audio disconnected \(streams.map(String.init).joined(separator: ","))")
        #endif
    }
}

/// A stream's ring in the runtime, which the guest's `libaaudio` fills (ADR-0036, decision 5 as changed).
final class RuntimeAudioSource: AudioRenderSource {
    let stream: UInt64

    init(stream: UInt64) {
        self.stream = stream
    }

    /// On the real-time thread: `apkrt_audio_read` never blocks, takes no lock and allocates nothing.
    func read(into buffer: UnsafeMutablePointer<Float>, frames: Int, channels: Int, hostTime: UInt64) -> Int {
        Int(apkrt_audio_read(stream, buffer, Int64(frames), hostTime))
    }
}
