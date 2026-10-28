#if BENCHMARK
import APKBench
import APKLibrary
import APKSession
import Darwin
import UIKit

/// What a benchmark run (ADR-0037, decision 5) and a recorded session (ADR-0041 d) share: the host's clock, the
/// conditions around what they measure, and the layer's recording (decision 2).
@MainActor
enum BenchmarkConditions {
    /// Host time, the guest's CLOCK_MONOTONIC (F10), and the display link's clock.
    nonisolated static func now() -> Int64 { Int64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) }

    nonisolated static func thermal() -> BenchThermal {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: .nominal
        case .fair: .fair
        case .serious: .serious
        default: .critical
        }
    }

    /// M6's MetalFX plan, step 0: the render scale in percent of ADR-0037's scene, from the launch argument
    /// `-benchmarkScale <n>` (it arrives as UserDefaults' "benchmarkScale"), 25 to 100; 100 without it, with a value
    /// outside, and always for test 2's Godot scene, whose bench.gd takes no scale. A by-hand measure: nothing records
    /// the scale in run.csv, so a scaled run is never a verdict's.
    nonisolated static func renderScale(for scene: BenchScene) -> Int {
        let scale = UserDefaults.standard.integer(forKey: "benchmarkScale")
        return scene == .parity && (25...100).contains(scale) ? scale : 100
    }

    /// M6's MetalFX plan, step 0: `-benchmarkLoad fill` runs ADR-0037's scene with the fill-heavy load, a by-hand
    /// measure; nil without it and for test 2's Godot scene.
    nonisolated static func loadOverride(for scene: BenchScene) -> String? {
        scene == .parity && UserDefaults.standard.string(forKey: "benchmarkLoad") == "fill" ? "fill" : nil
    }

    static func maxFPS() -> Int {
        UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.screen }.first?.maximumFramesPerSecond
            ?? 0
    }

    static func commit() -> String {
        Bundle.main.object(forInfoDictionaryKey: "MinatoCommit") as? String ?? ""
    }

    static func batteryState() -> String {
        switch UIDevice.current.batteryState {
        case .unplugged: "unplugged"
        case .charging: "charging"
        case .full: "full"
        default: "unknown"
        }
    }

    /// Whether a debugger traces the process (P_TRACED), as Apple's Technical Q&A QA1361 checks it.
    static func isTraced() -> Bool {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var name: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&name, UInt32(name.count), &info, &size, nil, 0) == 0 else { return false }
        return info.kp_proc.p_flag & P_TRACED != 0
    }

    /// The library's `entry` as a tap would start it: its native code signed first when the signer has not
    /// signed it yet. `name` names it in the errors.
    static func startable(_ entry: LibraryEntry, named name: String, library: Library,
                          signing: SigningCoordinator) async throws -> LibraryEntry {
        let signer = signing.currentSigner()
        switch LaunchDecision(entry: entry, isUpdating: false, signer: signer,
                              containerVersion: ContainerSigner.containerVersion) {
        case .start:
            return entry
        case .signFirst:
            guard let fingerprint = signer.fingerprint, let converter = signing.converter(for: signer) else {
                throw BenchmarkError("nothing signs \(name)'s native code")
            }
            _ = try await library.resign(id: entry.id, fingerprint: fingerprint, with: converter)
            return await library.entries.first { $0.id == entry.id } ?? entry
        case let decision:
            throw BenchmarkError("\(name) cannot start: \(decision)")
        }
    }

    /// Decision 3: a black screen until the thermal state is nominal, at most 10 minutes.
    static func waitUntilCool() async {
        let deadline = ContinuousClock.now + .seconds(BenchPlan.thermalWaitSeconds)
        while ProcessInfo.processInfo.thermalState != .nominal && ContinuousClock.now < deadline {
            try? await Task.sleep(for: .seconds(1))
        }
    }

    /// Observes the thermal state, Low Power Mode and the app's focus; each handler runs on the main thread.
    static func observe(thermal: @escaping @MainActor (BenchRunRecord.ThermalChange) -> Void,
                        lowPower: @escaping @MainActor (Bool) -> Void,
                        focusLoss: @escaping @MainActor (Int64) -> Void) -> [NSObjectProtocol] {
        let center = NotificationCenter.default
        return [
            center.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { _ in
                let change = BenchRunRecord.ThermalChange(timeNs: now(), state: Self.thermal())
                MainActor.assumeIsolated { thermal(change) }
            },
            center.addObserver(forName: .NSProcessInfoPowerStateDidChange, object: nil, queue: .main) { _ in
                let low = ProcessInfo.processInfo.isLowPowerModeEnabled
                MainActor.assumeIsolated { lowPower(low) }
            },
            center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in
                let time = now()
                MainActor.assumeIsolated { focusLoss(time) }
            },
        ]
    }

    /// Decision 2: the layer records each drawable from here on; false when it records nothing, as the
    /// Simulator's layer.
    static func startRecording(_ layer: CAMetalLayer, capacity: Int) -> Bool {
        if let layer = layer as? MinatoMetalLayer { return layer.startRecording(UInt(capacity)) }
        // Test 2's twin draws on Godot's own layer class, which MinatoLayerRecorder records (ADR-0042, decision 4).
        return MinatoLayerRecorder.startRecording(layer, capacity: UInt(capacity))
    }

    /// The recording, with the presented handlers that had not run; without one, the layer's values.
    static func stopRecording(_ layer: CAMetalLayer?) -> (recording: BenchRecording, unanswered: Int) {
        guard let layer else {
            return (BenchRecording(qos: "", pixelFormat: 0, drawableWidth: 0, drawableHeight: 0, maxDrawableCount: 0,
                                   drawables: [], lost: 0), 0)
        }
        let recording = if let layer = layer as? MinatoMetalLayer {
            layer.stopRecording(2)
        } else {
            MinatoLayerRecorder.stopRecording(layer, timeout: 2)
        }
        guard let recording else {
            return (BenchRecording(qos: "", pixelFormat: layer.pixelFormat.rawValue,
                                   drawableWidth: Int(layer.drawableSize.width),
                                   drawableHeight: Int(layer.drawableSize.height),
                                   maxDrawableCount: layer.maximumDrawableCount, drawables: [], lost: 0), 0)
        }
        let drawables = (0..<recording.count).map { index in
            let drawable = recording.drawable(index)
            return BenchDrawable(nextDrawableNs: drawable.next_drawable_ns, presentedNs: drawable.presented_ns)
        }
        return (BenchRecording(qos: recording.qosName, pixelFormat: recording.pixelFormat.rawValue,
                               drawableWidth: Int(recording.drawableSize.width),
                               drawableHeight: Int(recording.drawableSize.height),
                               maxDrawableCount: Int(recording.maximumDrawableCount), drawables: drawables,
                               lost: Int(recording.lost)), Int(recording.unanswered))
    }
}

/// The display link's intervals, for their median period (decision 6).
final class PeriodSink: FrameTickSink, @unchecked Sendable {
    private let lock = NSLock()
    private var last: Int64?
    private var intervals: [Int64] = []

    func tick(_ tick: FrameTick) {
        lock.withLock {
            if let last { intervals.append(tick.time - last) }
            last = tick.time
        }
    }

    /// The median by nearest rank, 0 without intervals.
    func median() -> Int64 {
        let sorted = lock.withLock { intervals.sorted() }
        guard !sorted.isEmpty else { return 0 }
        return sorted[(sorted.count + 1) / 2 - 1]
    }
}
#endif
