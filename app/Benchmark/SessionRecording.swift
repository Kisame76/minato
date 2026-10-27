#if BENCHMARK
import APKBench
import APKLibrary
import APKSession
import Darwin
import SwiftUI

extension BenchmarkMode {
    /// -recordSession <package> (ADR-0041 d), the App's part: the library's app runs in game mode's session in
    /// this process, with its drawables recorded from before its start to its end. The files go to
    /// tmp/benchmark/session-<package>/, where tools/record-session.sh fetches them.
    static let recordSession: String? = {
        let arguments = ProcessInfo.processInfo.arguments
        guard let flag = arguments.firstIndex(of: "-recordSession"), arguments.indices.contains(flag + 1) else {
            return nil
        }
        return arguments[flag + 1]
    }()
}

/// The screen of a recorded session: black until the app starts, without the menu, the FPS label or the
/// first-start hint, kept on.
struct SessionRecordingView: View {
    @State private var recorder: SessionRecorder

    init(package: String) {
        _recorder = State(initialValue: SessionRecorder(package: package))
    }

    var body: some View {
        ZStack {
            Color.black
            if let session = recorder.session {
                GameModeView(session: session)
            }
        }
        .ignoresSafeArea()
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .task {
            UIApplication.shared.isIdleTimerDisabled = true
            await recorder.start()
        }
    }
}

/// One recorded session: the conditions around it as a run's (ADR-0037, decision 5), then drawables.csv and
/// session.csv, then the end of the process.
@MainActor
@Observable
final class SessionRecorder {
    /// How long a session may take before its files are written without its end.
    static let timeLimitSeconds = 600
    /// How long a lifecycle that ended waits for the program's exit status.
    static let exitWaitSeconds = 2

    private(set) var session: GameSession?

    @ObservationIgnored private let package: String
    @ObservationIgnored private var record: BenchSessionRecord
    @ObservationIgnored private let periods = PeriodSink()
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private let folder: URL
    @ObservationIgnored private var layer: CAMetalLayer?
    /// The SurfaceViews' layers, recorded beside the window's.
    @ObservationIgnored private var surfaceLayers: [CAMetalLayer] = []
    @ObservationIgnored private var recording = false
    @ObservationIgnored private var notes: [String] = []
    @ObservationIgnored private var hasEnded = false

    init(package: String) {
        self.package = package
        record = BenchSessionRecord(package: package)
        folder = URL.temporaryDirectory.appending(path: "benchmark/session-\(package)", directoryHint: .isDirectory)
    }

    func start() async {
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let entry: LibraryEntry
        do {
            entry = try await startableEntry()
        } catch {
            return end("the session cannot be recorded: \(error)")
        }
        // Signing comes first, so that work has cooled off before the session.
        await BenchmarkConditions.waitUntilCool()
        observers = BenchmarkConditions.observe(
            thermal: { self.record.thermalChanges.append($0) },
            lowPower: { self.record.lowPower = self.record.lowPower || $0 },
            focusLoss: { _ in })
        record.maxFPS = BenchmarkConditions.maxFPS()
        record.thermalStart = BenchmarkConditions.thermal()
        record.lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        UIDevice.current.isBatteryMonitoringEnabled = true
        record.batteryState = BenchmarkConditions.batteryState()
        record.batteryLevel = UIDevice.current.batteryLevel
        record.commit = BenchmarkConditions.commit()
        record.configuration = "Release"
        record.debugger = BenchmarkConditions.isTraced()

        let session = GameSession(entry: entry)
        session.runBenchmark(BenchmarkHooks(ticks: periods, layer: { [weak self] in self?.startRecording($0) },
                                            surface: { [weak self] in self?.recordSurface($0) },
                                            ended: { [weak self] in self?.ended(status: $0, reason: $1) }))
        self.session = session
        // A session that never ends still gets its files.
        try? await Task.sleep(for: .seconds(Self.timeLimitSeconds))
        end("the session did not end within \(Self.timeLimitSeconds) s")
    }

    /// The library's app, ready and signed as a tap would start it.
    private func startableEntry() async throws -> LibraryEntry {
        let library = try Library(paths: try LibraryPaths.standard())
        guard let entry = await library.entries.first(where: { $0.id == package }) else {
            throw BenchmarkError("\(package) is not in the library")
        }
        let signing = SigningCoordinator(library: nil)
        await signing.refresh()
        return try await BenchmarkConditions.startable(entry, named: package, library: library, signing: signing)
    }

    /// Decision 2: the layer records each drawable from here on, before the guest starts. Not in the Simulator,
    /// whose layer records nothing.
    private func startRecording(_ layer: CAMetalLayer) {
        guard self.layer == nil else { return }
        self.layer = layer
        recording = BenchmarkConditions.startRecording(layer, capacity: BenchPlan.sessionRecordingCapacity)
        if !recording {
            notes.append("no recording (the Simulator's layer records nothing)")
        }
    }

    /// A SurfaceView's layer (ADR-0043 c4): the guest may present its frames there instead, as Godot does, so it
    /// records too, and the session takes the layer with the most drawables.
    private func recordSurface(_ layer: CAMetalLayer) {
        guard !hasEnded,
              BenchmarkConditions.startRecording(layer, capacity: BenchPlan.sessionRecordingCapacity) else { return }
        surfaceLayers.append(layer)
    }

    /// The session ended. A lifecycle that ended by itself waits a little for the program's exit status, which
    /// comes after it.
    private func ended(status: Int32?, reason: String?) {
        guard status == nil, reason == nil else { return end(reason, status: status) }
        Task {
            try? await Task.sleep(for: .seconds(Self.exitWaitSeconds))
            end(nil)
        }
    }

    /// The recording and the files, then the process ends. Only the first call counts.
    private func end(_ reason: String?, status: Int32? = nil) {
        guard !hasEnded else { return }
        hasEnded = true
        if let reason { notes.insert(reason, at: 0) }
        record.exitStatus = status
        record.linkPeriodMedianNs = periods.median()
        record.debugger = record.debugger || BenchmarkConditions.isTraced()
        let stopped = ([layer] + surfaceLayers.map(Optional.some)).map { BenchmarkConditions.stopRecording($0) }
        let unanswered = stopped.map(\.unanswered).reduce(0, +)
        // Never nil: the window's entry is always there.
        let (recorded, others) = BenchRecording.presenting(stopped.map(\.recording)) ?? (stopped[0].recording, 0)
        if others > 0 { notes.append("\(others) drawables on the session's other layers") }
        record.take(recorded)
        record.unanswered = unanswered
        if unanswered > 0 {
            notes.append("\(unanswered) presented handlers did not run")
        }
        // guestPresents counts the window's presents, so only a recording of the window's layer is compared.
        if recording, recorded == stopped[0].recording, let session,
           let note = BenchSessionRecord.drawableNote(drawables: recorded.drawables.count, lost: recorded.lost,
                                                      presents: session.guestPresents) {
            notes.append(note)
        }
        if recorded.lost > 0 {
            notes.append("\(recorded.lost) drawables past the recording's \(BenchPlan.sessionRecordingCapacity)")
        }
        try? BenchCSV.drawables(recorded.drawables).write(to: folder.appending(path: "drawables.csv"),
                                                          atomically: true, encoding: .utf8)
        try? BenchCSV.session(record).write(to: folder.appending(path: "session.csv"), atomically: true,
                                            encoding: .utf8)
        if !notes.isEmpty {
            try? (notes.joined(separator: "; ") + "\n").write(to: folder.appending(path: "error"), atomically: true,
                                                              encoding: .utf8)
        }
        // Without exit's static destructors, which would run under the app's threads (the end handler, apkrt.h).
        fflush(nil)
        _exit(0)
    }
}
#endif
