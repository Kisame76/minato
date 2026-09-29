#if BENCHMARK
import APKBench
import APKLibrary
import APKSession
import CryptoKit
import Darwin
import SwiftUI

/// The benchmark mode (ADR-0037, decision 3), only in builds with APK_EMBED_BENCHMARK=YES: launched with
/// -runBenchmark <run>, the app shows a black screen, waits until the thermal state is nominal, runs the
/// variant and writes the run's CSVs to tmp/benchmark/<run>/, where tools/benchmark.sh fetches them.
/// -benchmarkSeconds <n> shortens the draw time for the Simulator.
enum BenchmarkMode {
    static let run: BenchRun? = {
        let arguments = ProcessInfo.processInfo.arguments
        guard let flag = arguments.firstIndex(of: "-runBenchmark"), arguments.indices.contains(flag + 1) else {
            return nil
        }
        return BenchRun(arguments[flag + 1])
    }()

    static let seconds: Int = {
        let arguments = ProcessInfo.processInfo.arguments
        guard let flag = arguments.firstIndex(of: "-benchmarkSeconds"), arguments.indices.contains(flag + 1),
            let seconds = Int(arguments[flag + 1]), seconds > 0
        else { return BenchPlan.drawSeconds }
        return seconds
    }()
}

/// What a run needs of game mode's session (GameSession.runBenchmark): the frame clock's ticks, the layer
/// before the guest starts, and the variant's end instead of the way back to the library.
@MainActor
struct BenchmarkHooks {
    let ticks: FrameTickSink
    let layer: (CAMetalLayer) -> Void
    /// nil, or why the variant did not end well.
    let ended: (String?) -> Void
}

/// Hands each tick to two sinks.
final class TickTee: FrameTickSink, @unchecked Sendable {
    private let first: FrameTickSink
    private let second: FrameTickSink

    init(_ first: FrameTickSink, _ second: FrameTickSink) {
        self.first = first
        self.second = second
    }

    func tick(_ tick: FrameTick) {
        first.tick(tick)
        second.tick(tick)
    }
}

/// The screen of a run: black, without the menu, the FPS label or the first-start hint, kept on.
struct BenchmarkView: View {
    @State private var runner: BenchmarkRunner

    init(run: BenchRun) {
        _runner = State(initialValue: BenchmarkRunner(run: run, seconds: BenchmarkMode.seconds))
    }

    var body: some View {
        ZStack {
            Color.black
            switch runner.stage {
            case .waiting:
                EmptyView()
            case .native:
                MetalLayerView(onLayer: { runner.nativeLayer($0) }, onTopEdgeSwipe: {},
                               onLayout: { runner.nativeLaidOut($0) }, onTouches: { _, _, _ in })
            case .apk(let session):
                GameModeView(session: session)
            }
        }
        .ignoresSafeArea()
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .task {
            UIApplication.shared.isIdleTimerDisabled = true
            await runner.start()
        }
    }
}

/// One run: the conditions around the variant (decision 5), then the CSVs, then the end of the process.
@MainActor
@Observable
final class BenchmarkRunner {
    enum Stage {
        case waiting
        /// The scene built for iOS, on the view's layer.
        case native
        /// The APK, in game mode's session.
        case apk(GameSession)
    }

    private(set) var stage = Stage.waiting

    @ObservationIgnored private let run: BenchRun
    @ObservationIgnored private let seconds: Int
    @ObservationIgnored private var record: BenchRunRecord
    @ObservationIgnored private let periods = PeriodSink()
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private let folder: URL
    @ObservationIgnored private var bench: MinatoBench?
    @ObservationIgnored private var layer: CAMetalLayer?
    @ObservationIgnored private var clock: DisplayLinkClock?
    @ObservationIgnored private var nativeStarted = false
    /// The APK's scene.csv, beside its bench.conf in internalDataPath.
    @ObservationIgnored private var apkScene: URL?
    @ObservationIgnored private var notes: [String] = []
    @ObservationIgnored private var hasEnded = false

    init(run: BenchRun, seconds: Int) {
        self.run = run
        self.seconds = seconds
        record = BenchRunRecord(run: run)
        folder = URL.temporaryDirectory.appending(path: "benchmark/\(run)", directoryHint: .isDirectory)
    }

    func start() async {
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        // The APK is imported and signed first, so that work has cooled off before the run.
        var session: GameSession?
        if run.variant == .apk {
            do {
                session = GameSession(entry: try await prepareAPK())
            } catch {
                return end("the APK could not be prepared: \(error)")
            }
        } else {
            do {
                bench = try MinatoBench.open()
            } catch {
                return end("\(error)")
            }
        }
        await waitUntilCool()
        observe()
        let rate = run.configuration.rate
        record.requestedRate = Float(rate)
        record.maxFPS = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.screen }
            .first?.maximumFramesPerSecond ?? 0
        record.thermalStart = Self.thermal()
        record.lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        UIDevice.current.isBatteryMonitoringEnabled = true
        record.batteryState = Self.batteryState()
        record.batteryLevel = UIDevice.current.batteryLevel
        record.commit = Bundle.main.object(forInfoDictionaryKey: "MinatoCommit") as? String ?? ""
        record.configuration = "Release"
        record.debugger = Self.isTraced()
        record.startNs = Self.now()

        if let session {
            // The APK asks for the rate itself (ANativeWindow_setFrameRate), on game mode's frame clock.
            session.runBenchmark(BenchmarkHooks(ticks: periods, layer: { [weak self] in self?.startRecording($0) },
                                                ended: { [weak self] in self?.end($0) }))
            stage = .apk(session)
        } else {
            stage = .native
        }
        // A variant that never ends still gets its CSVs.
        try? await Task.sleep(for: .seconds(seconds + 60))
        end("the variant did not end within \(seconds + 60) s")
    }

    // MARK: - The native variant

    /// The view's layer, before the scene draws.
    func nativeLayer(_ layer: CAMetalLayer) {
        startRecording(layer)
    }

    /// The scene starts at the view's first layout with a size, on the layer at that size.
    func nativeLaidOut(_ pixels: CGSize) {
        guard !nativeStarted, !hasEnded, pixels.width > 0, pixels.height > 0, let layer, let bench else { return }
        nativeStarted = true
        let scene = folder.appending(path: "scene.csv")
        let moltenvk = Bundle.main.privateFrameworksURL?.appending(path: "MoltenVK.framework/MoltenVK").path
        var error = [CChar](repeating: 0, count: 512)
        let started = bench.start(run.configuration.api.rawValue, BenchConf.text(for: run, seconds: seconds),
                                  Unmanaged.passUnretained(layer).toOpaque(), moltenvk, scene.path, &error, error.count)
        guard started == 0 else { return end("the scene did not start: \(MinatoBench.text(error))") }
        let clock = DisplayLinkClock(sink: TickTee(periods, NativeTicks(bench.tick)))
        clock.setPreferredRate(Float(run.configuration.rate))
        clock.run(true)
        self.clock = clock
        let limit = seconds + 30
        Task.detached {
            var reason: String? = "the scene did not end within \(limit) s"
            var error = [CChar](repeating: 0, count: 512)
            for _ in 0..<limit {
                let status = bench.wait(1, &error, error.count)
                if status == 1 { continue }
                reason = status == 0 ? nil : "the scene failed: \(MinatoBench.text(error))"
                break
            }
            await MainActor.run { [reason] in self.end(reason) }
        }
    }

    // MARK: - The APK variant

    /// The API's APK from the bundle's Benchmark/, installed at the first start like any APK (and again when the
    /// build's APK differs), its native code signed by the signer a tap would use; bench.conf in its
    /// internalDataPath.
    private func prepareAPK() async throws -> LibraryEntry {
        let api = run.configuration.api
        guard let apk = Bundle.main.resourceURL?.appending(path: "Benchmark/\(api.apkFile)"),
            FileManager.default.fileExists(atPath: apk.path)
        else { throw BenchmarkError("\(api.apkFile) is not in this build") }
        let paths = try LibraryPaths.standard()
        let library = try Library(paths: paths)
        let signing = SigningCoordinator(library: nil)
        await signing.refresh()

        let sha256 = SHA256.hash(data: try Data(contentsOf: apk)).map { String(format: "%02x", $0) }.joined()
        var entry = await library.entries.first { $0.id == api.package }
        if entry?.source.sha256 != sha256 {
            let incoming = try await library.adopt(apk)
            entry = try await library.importFile(incoming, replacing: true, signing: signing.importSigning()).entry
        }
        guard var entry, entry.id == api.package, entry.source.sha256 == sha256 else {
            throw BenchmarkError("\(api.apkFile) was not installed")
        }
        let signer = signing.currentSigner()
        switch LaunchDecision(entry: entry, isUpdating: false, signer: signer) {
        case .start:
            break
        case .signFirst:
            guard let fingerprint = signer.fingerprint, let converter = signing.converter(for: signer) else {
                throw BenchmarkError("nothing signs \(api.apkFile)'s native code")
            }
            _ = try await library.resign(id: entry.id, fingerprint: fingerprint, with: converter)
            entry = await library.entries.first { $0.id == api.package } ?? entry
        case let decision:
            throw BenchmarkError("\(api.apkFile) cannot start: \(decision)")
        }

        // internalDataPath: /data/user/0/<package>/files (GuestStart's paths).
        let files = paths.dataDirectory(for: entry.id).appending(path: "data/files", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: files, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: files.appending(path: "scene.csv"))
        try BenchConf.text(for: run, seconds: seconds).write(to: files.appending(path: "bench.conf"), atomically: true,
                                                            encoding: .utf8)
        apkScene = files.appending(path: "scene.csv")
        return entry
    }

    // MARK: - The recording and the end

    /// Decision 2: the layer records each drawable from here on. Not in the Simulator, whose layer records
    /// nothing: the run then has no drawables and is invalid.
    private func startRecording(_ layer: CAMetalLayer) {
        guard self.layer == nil else { return }
        self.layer = layer
        let capacity = BenchPlan.recordingCapacity(seconds: seconds, rate: run.configuration.rate)
        if (layer as? MinatoMetalLayer)?.startRecording(UInt(capacity)) != true {
            notes.append("no recording (the Simulator's layer records nothing)")
        }
    }

    /// The variant ended, or failed with `reason`: the recording, the scene's frames and the CSVs, then the
    /// process ends. Only the first call counts.
    private func end(_ reason: String?) {
        guard !hasEnded else { return }
        hasEnded = true
        clock?.run(false)
        if let reason { notes.insert(reason, at: 0) }
        record.linkPeriodMedianNs = periods.median()
        record.debugger = record.debugger || Self.isTraced()
        let recording = stopRecording()
        record.take(recording)

        let sceneFile = apkScene ?? folder.appending(path: "scene.csv")
        var scene: [BenchSceneFrame] = []
        if let text = try? String(contentsOf: sceneFile, encoding: .utf8) {
            if sceneFile != folder.appending(path: "scene.csv") {
                try? text.write(to: folder.appending(path: "scene.csv"), atomically: true, encoding: .utf8)
            }
            do { scene = try BenchCSV.scene(text) } catch { notes.append("scene.csv: \(error)") }
        } else if nativeStarted || stage.isAPK {
            notes.append("no scene.csv")
        }
        record.sceneFrames = scene.count
        let configuration = run.configuration
        if let meshes = try? bench ?? MinatoBench.open() {
            record.meshes = Int(meshes.meshes(configuration.api.rawValue, configuration.load.rawValue))
        }
        write(frames: BenchCSV.join(drawables: recording.drawables, scene: scene))
        exit(0)
    }

    private func stopRecording() -> BenchRecording {
        guard let layer else {
            return BenchRecording(qos: "", pixelFormat: 0, drawableWidth: 0, drawableHeight: 0, maxDrawableCount: 0,
                                  drawables: [], lost: 0)
        }
        guard let recording = (layer as? MinatoMetalLayer)?.stopRecording(2) else {
            return BenchRecording(qos: "", pixelFormat: layer.pixelFormat.rawValue,
                                  drawableWidth: Int(layer.drawableSize.width),
                                  drawableHeight: Int(layer.drawableSize.height),
                                  maxDrawableCount: layer.maximumDrawableCount, drawables: [], lost: 0)
        }
        if recording.unanswered > 0 {
            notes.append("\(recording.unanswered) presented handlers did not run")
        }
        let drawables = (0..<recording.count).map { index in
            let drawable = recording.drawable(index)
            return BenchDrawable(nextDrawableNs: drawable.next_drawable_ns, presentedNs: drawable.presented_ns)
        }
        return BenchRecording(qos: recording.qosName, pixelFormat: recording.pixelFormat.rawValue,
                              drawableWidth: Int(recording.drawableSize.width),
                              drawableHeight: Int(recording.drawableSize.height),
                              maxDrawableCount: Int(recording.maximumDrawableCount), drawables: drawables,
                              lost: Int(recording.lost))
    }

    /// Decision 3: a black screen until the thermal state is nominal, at most 10 minutes.
    private func waitUntilCool() async {
        let deadline = ContinuousClock.now + .seconds(BenchPlan.thermalWaitSeconds)
        while ProcessInfo.processInfo.thermalState != .nominal && ContinuousClock.now < deadline {
            try? await Task.sleep(for: .seconds(1))
        }
    }

    private func observe() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil,
                                            queue: .main) { _ in
            let change = BenchRunRecord.ThermalChange(timeNs: Self.now(), state: Self.thermal())
            MainActor.assumeIsolated { self.record.thermalChanges.append(change) }
        })
        observers.append(center.addObserver(forName: .NSProcessInfoPowerStateDidChange, object: nil,
                                            queue: .main) { _ in
            let low = ProcessInfo.processInfo.isLowPowerModeEnabled
            MainActor.assumeIsolated { self.record.lowPower = self.record.lowPower || low }
        })
        observers.append(center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil,
                                            queue: .main) { _ in
            let time = Self.now()
            MainActor.assumeIsolated { self.record.focusLosses.append(time) }
        })
    }

    private func write(frames: [BenchFrame]) {
        try? BenchCSV.frames(frames).write(to: folder.appending(path: "frames.csv"), atomically: true, encoding: .utf8)
        try? BenchCSV.run(record).write(to: folder.appending(path: "run.csv"), atomically: true, encoding: .utf8)
        if !notes.isEmpty {
            try? (notes.joined(separator: "; ") + "\n").write(to: folder.appending(path: "error"), atomically: true,
                                                              encoding: .utf8)
        }
    }

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

    private static func batteryState() -> String {
        switch UIDevice.current.batteryState {
        case .unplugged: "unplugged"
        case .charging: "charging"
        case .full: "full"
        default: "unknown"
        }
    }

    /// Whether a debugger traces the process (P_TRACED), as Apple's Technical Q&A QA1361 checks it.
    private static func isTraced() -> Bool {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var name: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&name, UInt32(name.count), &info, &size, nil, 0) == 0 else { return false }
        return info.kp_proc.p_flag & P_TRACED != 0
    }
}

extension BenchmarkRunner.Stage {
    var isAPK: Bool {
        if case .apk = self { true } else { false }
    }
}

struct BenchmarkError: Error, CustomStringConvertible {
    let description: String

    init(_ description: String) {
        self.description = description
    }
}

/// MinatoBench.framework (graphics/bench/native/minato_bench.h), opened by dlopen: only benchmark builds embed it.
struct MinatoBench: @unchecked Sendable {
    typealias Start = @convention(c) (UnsafePointer<CChar>, UnsafePointer<CChar>, UnsafeMutableRawPointer,
                                      UnsafePointer<CChar>?, UnsafePointer<CChar>, UnsafeMutablePointer<CChar>,
                                      Int) -> Int32
    typealias Tick = @convention(c) (Int64) -> Void
    typealias Wait = @convention(c) (Double, UnsafeMutablePointer<CChar>, Int) -> Int32
    typealias Meshes = @convention(c) (UnsafePointer<CChar>, UnsafePointer<CChar>) -> Int32

    let start: Start
    let tick: Tick
    let wait: Wait
    let meshes: Meshes

    /// A reason the library wrote into `buffer`.
    static func text(_ buffer: [CChar]) -> String {
        String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    static func open() throws -> MinatoBench {
        let path = Bundle.main.privateFrameworksURL?.appending(path: "MinatoBench.framework/MinatoBench").path ?? ""
        guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
            let reason = dlerror().map { String(cString: $0) } ?? "no reason"
            throw BenchmarkError("MinatoBench.framework could not be opened: \(reason)")
        }
        func symbol<T>(_ name: String, as type: T.Type) throws -> T {
            guard let address = dlsym(handle, name) else { throw BenchmarkError("MinatoBench has no \(name)") }
            return unsafeBitCast(address, to: type)
        }
        return MinatoBench(start: try symbol("minato_bench_start", as: Start.self),
                           tick: try symbol("minato_bench_tick", as: Tick.self),
                           wait: try symbol("minato_bench_wait", as: Wait.self),
                           meshes: try symbol("minato_bench_meshes", as: Meshes.self))
    }
}

/// The display link's ticks to the scene's render thread.
private final class NativeTicks: FrameTickSink, @unchecked Sendable {
    private let tick: MinatoBench.Tick

    init(_ tick: @escaping MinatoBench.Tick) {
        self.tick = tick
    }

    func tick(_ tick: FrameTick) {
        self.tick(tick.time)
    }
}

/// The display link's intervals during a run, for its median period (decision 6).
private final class PeriodSink: FrameTickSink, @unchecked Sendable {
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
