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
/// -benchmarkSeconds <n> shortens the draw time for the Simulator. Each -benchmarkGuestEnvironment <KEY=VALUE>
/// adds an entry to the APK variant's guest environment, such as libvulkan's MINATO_VULKAN_BGRA8_FIRST=1
/// (ADR-0030 V5).
enum BenchmarkMode {
    static let run: BenchRun? = BenchLaunch.run(in: ProcessInfo.processInfo.arguments)

    static let seconds: Int = {
        let arguments = ProcessInfo.processInfo.arguments
        guard let flag = arguments.firstIndex(of: "-benchmarkSeconds"), arguments.indices.contains(flag + 1),
            let seconds = Int(arguments[flag + 1]), seconds > 0
        else { return BenchPlan.drawSeconds }
        return seconds
    }()

    static let guestEnvironment: [String] = {
        let arguments = ProcessInfo.processInfo.arguments
        return arguments.indices.dropLast().filter { arguments[$0] == "-benchmarkGuestEnvironment" }
            .map { arguments[$0 + 1] }
    }()
}

/// What a run needs of game mode's session (GameSession.runBenchmark): the frame clock's ticks, the window's layer
/// before the guest starts, each SurfaceView's layer as the guest makes it (ADR-0043 c4), and the variant's end
/// instead of the way back to the library.
@MainActor
struct BenchmarkHooks {
    let ticks: FrameTickSink
    let layer: (CAMetalLayer) -> Void
    let surface: (CAMetalLayer) -> Void
    /// The program's exit status, nil when it did not end by itself; then nil, or why the variant did not end
    /// well.
    let ended: (Int32?, String?) -> Void
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
                MetalLayerView(onLayer: { layer, _ in runner.nativeLayer(layer) }, onTopEdgeSwipe: {},
                               onLayout: { runner.nativeLaidOut($0) }, onTouches: { _, _, _ in })
            case .apk(let session):
                GameModeView(session: session)
            case .twin(let controller):
                GodotTwinView(controller: controller)
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
        /// Test 2's twin: Godot's view controller (ADR-0042, decision 3).
        case twin(UIViewController)
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
    /// The APK's SurfaceViews' layers, recorded beside the window's.
    @ObservationIgnored private var surfaceLayers: [CAMetalLayer] = []
    @ObservationIgnored private var clock: DisplayLinkClock?
    @ObservationIgnored private var nativeStarted = false
    /// The APK's scene.csv, beside its bench.conf in internalDataPath.
    @ObservationIgnored private var apkScene: URL?
    @ObservationIgnored private var notes: [String] = []
    @ObservationIgnored private var hasEnded = false
    /// Test 2's twin: its view controller, and whether Godot has drawn bench.gd's frame 0.
    @ObservationIgnored private var twinController: UIViewController?
    @ObservationIgnored private var twinDrawing = false

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
        var twin: GodotTwin?
        if run.variant == .apk {
            do {
                session = GameSession(entry: try await prepareAPK())
            } catch {
                return end("the APK could not be prepared: \(error)")
            }
        } else if run.variant == .twin {
            #if targetEnvironment(simulator)
            return end("device only: the twin has no Simulator slice (ADR-0042, departure 2)")
            #else
            do {
                twin = try GodotTwin.open()
            } catch {
                return end("\(error)")
            }
            #endif
        } else {
            do {
                bench = try MinatoBench.open()
            } catch {
                return end("\(error)")
            }
        }
        await BenchmarkConditions.waitUntilCool()
        observe()
        let rate = run.configuration.rate
        record.requestedRate = Float(rate)
        record.maxFPS = BenchmarkConditions.maxFPS()
        record.thermalStart = BenchmarkConditions.thermal()
        record.lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        UIDevice.current.isBatteryMonitoringEnabled = true
        record.batteryState = BenchmarkConditions.batteryState()
        record.batteryLevel = UIDevice.current.batteryLevel
        record.commit = BenchmarkConditions.commit()
        record.configuration = "Release"
        record.debugger = BenchmarkConditions.isTraced()
        record.startNs = BenchmarkConditions.now()

        if let session {
            // The APK asks for the rate itself (ANativeWindow_setFrameRate), on game mode's frame clock.
            session.runBenchmark(BenchmarkHooks(ticks: periods, layer: { [weak self] in self?.startRecording($0) },
                                                surface: { [weak self] in self?.recordSurface($0) },
                                                ended: { [weak self] in self?.end($1) }))
            stage = .apk(session)
        } else if let twin {
            // Landscape before Godot starts, as the APK's session holds it, or the pair's layers differ (ADR-0042, d4).
            guard let scene = UIApplication.shared.connectedScenes.lazy.compactMap({ $0 as? UIWindowScene }).first
            else { return end("no window scene for the twin") }
            _ = AppDelegate.hold(.landscape, in: scene) {}
            guard await TwinOrientation.wait(isLandscape: { scene.effectiveGeometry.interfaceOrientation.isLandscape })
            else { return end(TwinOrientation.refused) }
            startTwin(twin)
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
        let scale = BenchmarkConditions.renderScale(for: run.configuration.scene)
        let load = BenchmarkConditions.loadOverride(for: run.configuration.scene)
        let text = BenchConf.text(for: run, seconds: seconds, scale: scale, load: load)
        let started = bench.start(run.configuration.api.rawValue, text,
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

    // MARK: - Test 2's twin

    /// bench.conf in the run's folder, where bench.gd writes scene.csv and done beside it; the recorder on Godot's
    /// layer class; the frame hooks; then Godot, before its view first draws. Godot paces its frames itself; the
    /// display link here measures the link's period (decision 6).
    private func startTwin(_ twin: GodotTwin) {
        let conf = folder.appending(path: "bench.conf")
        do {
            try BenchConf.text(for: run, seconds: seconds).write(to: conf, atomically: true, encoding: .utf8)
        } catch {
            return end("bench.conf: \(error)")
        }
        if run.configuration.api != .gles {
            guard let layerClass = NSClassFromString("GDTMetalLayer"), MinatoLayerRecorder.attach(to: layerClass)
            else { return end("the recorder could not attach to Godot's GDTMetalLayer") }
        }
        let context = Unmanaged.passUnretained(self).toOpaque()
        let hooked = twin.frameHooks({ context in
            MainActor.assumeIsolated { BenchmarkRunner.runner(context)?.twinBefore() }
        }, { context, frame, done in
            MainActor.assumeIsolated { BenchmarkRunner.runner(context)?.twinAfter(frame: frame, done: done != 0) }
        }, context)
        guard hooked == 0 else { return end("Godot's view has no drawView for the frame hooks") }
        let clock = DisplayLinkClock(sink: periods)
        clock.setPreferredRate(Float(run.configuration.rate))
        clock.run(true)
        self.clock = clock
        do {
            let controller = try twin.run(run.configuration.godotArguments + ["--bench-config=\(conf.path)"])
            twinController = controller
            stage = .twin(controller)
        } catch {
            end("\(error)")
        }
    }

    /// The runner behind the frame hooks' context, which the run's view keeps alive.
    nonisolated private static func runner(_ context: UnsafeMutableRawPointer?) -> BenchmarkRunner? {
        context.map { Unmanaged<BenchmarkRunner>.fromOpaque($0).takeUnretainedValue() }
    }

    /// Before each of Godot's frames: until frame 0 is drawn, a recording runs from here, so the one that holds
    /// frame 0 begins with it. The draws before set up the engine and show the boot image.
    /// Godot renders on the main thread (project.godot sets no thread_model), so no other thread is in nextDrawable
    /// while a recording starts or stops here.
    private func twinBefore() {
        guard !twinDrawing, !hasEnded, layer == nil, let godotLayer = twinLayer() else { return }
        guard startRecording(godotLayer) else {
            return end("the recorder did not start on Godot's \(type(of: godotLayer))")
        }
    }

    /// After each: a recording without frame 0 is let go; after the frame that wrote done, the run ends at once, so
    /// the recording holds exactly scene.csv's frames.
    private func twinAfter(frame: Int, done: Bool) {
        guard !hasEnded else { return }
        if !twinDrawing {
            guard frame >= 0 else {
                if let layer {
                    _ = MinatoLayerRecorder.stopRecording(layer, timeout: 0)
                    self.layer = nil
                }
                return
            }
            guard frame == 0 else { return end("Godot drew frame \(frame) before the first recorded one") }
            guard layer != nil || run.configuration.api == .gles else {
                return end("frame 0 was drawn without a recording on Godot's layer")
            }
            twinDrawing = true
        }
        if done { end(nil) }
    }

    /// Godot's Metal layer, a sublayer of its view's once Godot has set up its display; GLES draws on none.
    private func twinLayer() -> CAMetalLayer? {
        guard run.configuration.api != .gles, let view = twinController?.viewIfLoaded else { return nil }
        return view.layer.sublayers?.lazy.compactMap { $0 as? CAMetalLayer }.first
    }

    // MARK: - The APK variant

    /// The API's APK from the bundle's Benchmark/, installed at the first start like any APK (and again when the
    /// build's APK differs), its native code signed by the signer a tap would use; bench.conf in its
    /// internalDataPath.
    private func prepareAPK() async throws -> LibraryEntry {
        let configuration = run.configuration
        guard let apk = Bundle.main.resourceURL?.appending(path: "Benchmark/\(configuration.apkFile)"),
            FileManager.default.fileExists(atPath: apk.path)
        else { throw BenchmarkError("\(configuration.apkFile) is not in this build") }
        let paths = try LibraryPaths.standard()
        let library = try Library(paths: paths)
        let signing = SigningCoordinator(library: nil)
        await signing.refresh()

        let sha256 = SHA256.hash(data: try Data(contentsOf: apk)).map { String(format: "%02x", $0) }.joined()
        var entry = await library.entries.first { $0.id == configuration.package }
        if entry?.source.sha256 != sha256 {
            let incoming = try await library.adopt(apk)
            entry = try await library.importFile(incoming, replacing: true, signing: signing.importSigning()).entry
        }
        guard var entry, entry.id == configuration.package, entry.source.sha256 == sha256 else {
            throw BenchmarkError("\(configuration.apkFile) was not installed")
        }
        entry = try await BenchmarkConditions.startable(entry, named: configuration.apkFile, library: library,
                                                        signing: signing)

        // internalDataPath: /data/user/0/<package>/files (GuestStart's paths).
        let files = paths.dataDirectory(for: entry.id).appending(path: "data/files", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: files, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: files.appending(path: "scene.csv"))
        let scale = BenchmarkConditions.renderScale(for: run.configuration.scene)
        let load = BenchmarkConditions.loadOverride(for: run.configuration.scene)
        try BenchConf.text(for: run, seconds: seconds, scale: scale, load: load)
            .write(to: files.appending(path: "bench.conf"), atomically: true, encoding: .utf8)
        apkScene = files.appending(path: "scene.csv")
        return entry
    }

    // MARK: - The recording and the end

    /// Decision 2: the layer records each drawable from here on. Not in the Simulator, whose layer records
    /// nothing: the run then has no drawables and is invalid. False when this call started no recording.
    @discardableResult
    private func startRecording(_ layer: CAMetalLayer) -> Bool {
        guard self.layer == nil else { return false }
        self.layer = layer
        let capacity = BenchPlan.recordingCapacity(seconds: seconds, rate: run.configuration.rate)
        guard BenchmarkConditions.startRecording(layer, capacity: capacity) else {
            // The twin, which never runs in the Simulator, ends with its own reason.
            if run.variant != .twin { notes.append("no recording (the Simulator's layer records nothing)") }
            return false
        }
        return true
    }

    /// A SurfaceView's layer in the APK variant (ADR-0043 c4): the guest may draw its frames there instead, as Godot
    /// does, so it records too, and the run takes the layer with the most drawables.
    private func recordSurface(_ layer: CAMetalLayer) {
        guard !hasEnded else { return }
        let capacity = BenchPlan.recordingCapacity(seconds: seconds, rate: run.configuration.rate)
        if BenchmarkConditions.startRecording(layer, capacity: capacity) { surfaceLayers.append(layer) }
    }

    /// The variant ended, or failed with `reason`: the recording, the scene's frames and the CSVs, then the
    /// process ends with _exit. Only the first call counts.
    private func end(_ reason: String?) {
        guard !hasEnded else { return }
        hasEnded = true
        clock?.run(false)
        if let reason { notes.insert(reason, at: 0) }
        record.linkPeriodMedianNs = periods.median()
        // The APK asks for its rate itself: what it asked, 0 for none, which RunValidity judges by its frame times.
        if case .apk(let session) = stage { record.requestedRate = session.guestAskedRate }
        record.debugger = record.debugger || BenchmarkConditions.isTraced()
        let recording = stopRecording()
        record.take(recording)

        let sceneFile = apkScene ?? folder.appending(path: "scene.csv")
        var scene: [BenchSceneFrame] = []
        if let text = try? String(contentsOf: sceneFile, encoding: .utf8) {
            if sceneFile != folder.appending(path: "scene.csv") {
                try? text.write(to: folder.appending(path: "scene.csv"), atomically: true, encoding: .utf8)
            }
            do { scene = try BenchCSV.scene(text) } catch { notes.append("scene.csv: \(error)") }
        } else if nativeStarted || stage.isAPK || twinController != nil {
            notes.append("no scene.csv")
        }
        record.sceneFrames = scene.count
        let configuration = run.configuration
        var drawables = recording.drawables
        // Test 2's script counts its frames in the done file beside scene.csv.
        if configuration.scene == .godot {
            let done = sceneFile.deletingLastPathComponent().appending(path: "done")
            record.doneFrames = (try? String(contentsOf: done, encoding: .utf8)).flatMap(BenchDone.frames) ?? -1
            // Godot's APK draws its boot image on its surface before frame 0: its frames are the last drawables
            // (decision 4), which holds while the done frame is the last one.
            if run.variant == .apk {
                let aligned = recording.alignedAtEnd(frames: scene.count)
                drawables = aligned.drawables
                record.drawablesBeforeFrame0 = aligned.before
                record.firstPresentNs = drawables.first { $0.presentedNs > 0 }?.presentedNs ?? 0
            }
            if !drawables.isEmpty, let written = Self.uptimeNs(ofWriteOf: done) {
                record.drawablesAfterDone = recording.drawablesTaken(after: written)
            }
        }
        if let meshes = configuration.godotMeshes {
            record.meshes = meshes
        } else if let meshes = try? bench ?? MinatoBench.open() {
            // MinatoBench knows the scene's APIs: bench-gles31 draws bench-gles's meshes (ADR-0044 d2)).
            record.meshes = Int(meshes.meshes(configuration.api.sceneAPI.rawValue, configuration.load.rawValue))
        }
        write(frames: BenchCSV.join(drawables: drawables, scene: scene))
        // The CSVs are written and closed. The process ends without exit's static destructors, which would run under
        // Godot's live threads, the twin's (c4) or an APK's that ended by killing itself (the end handler, apkrt.h),
        // and abort on a destroyed mutex; stdio's buffers, Godot's last console lines, go out first.
        fflush(nil)
        _exit(0)
    }

    /// When `file` was last written, on the recording's clock (CLOCK_UPTIME_RAW); nil when it cannot be read.
    nonisolated private static func uptimeNs(ofWriteOf file: URL) -> Int64? {
        var status = stat()
        guard stat(file.path, &status) == 0 else { return nil }
        let written = Int64(status.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(status.st_mtimespec.tv_nsec)
        let offset = Int64(clock_gettime_nsec_np(CLOCK_REALTIME)) - BenchmarkConditions.now()
        return written - offset
    }

    /// The recording of the layer the guest presented on: the window's or a SurfaceView's.
    private func stopRecording() -> BenchRecording {
        let stopped = ([layer] + surfaceLayers.map(Optional.some)).map { BenchmarkConditions.stopRecording($0) }
        let unanswered = stopped.map(\.unanswered).reduce(0, +)
        if unanswered > 0 {
            notes.append("\(unanswered) presented handlers did not run")
        }
        // Never nil: the window's entry is always there.
        let (recording, others) = BenchRecording.presenting(stopped.map(\.recording)) ?? (stopped[0].recording, 0)
        if others > 0 { notes.append("\(others) drawables on the session's other layers") }
        return recording
    }

    private func observe() {
        observers = BenchmarkConditions.observe(
            thermal: { self.record.thermalChanges.append($0) },
            lowPower: { self.record.lowPower = self.record.lowPower || $0 },
            focusLoss: { self.record.focusLosses.append($0) })
    }

    private func write(frames: [BenchFrame]) {
        try? BenchCSV.frames(frames).write(to: folder.appending(path: "frames.csv"), atomically: true, encoding: .utf8)
        try? BenchCSV.run(record).write(to: folder.appending(path: "run.csv"), atomically: true, encoding: .utf8)
        if !notes.isEmpty {
            try? (notes.joined(separator: "; ") + "\n").write(to: folder.appending(path: "error"), atomically: true,
                                                              encoding: .utf8)
        }
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
#endif
