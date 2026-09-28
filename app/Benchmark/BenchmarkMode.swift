#if BENCHMARK
import APKBench
import APKSession
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

/// The screen of a run: black, without the menu, the FPS label or the first-start hint, kept on.
struct BenchmarkView: View {
    let run: BenchRun

    var body: some View {
        Color.black
            .ignoresSafeArea()
            .statusBarHidden()
            .persistentSystemOverlays(.hidden)
            .task {
                UIApplication.shared.isIdleTimerDisabled = true
                await BenchmarkRunner(run: run, seconds: BenchmarkMode.seconds).start()
            }
    }
}

/// One run: the conditions around the variant (decision 5), then the CSVs, then the end of the process.
@MainActor
final class BenchmarkRunner {
    private let run: BenchRun
    private let seconds: Int
    private var record: BenchRunRecord
    private let periods = PeriodSink()
    private var observers: [NSObjectProtocol] = []
    private let folder: URL

    init(run: BenchRun, seconds: Int) {
        self.run = run
        self.seconds = seconds
        record = BenchRunRecord(run: run)
        folder = URL.temporaryDirectory.appending(path: "benchmark/\(run)", directoryHint: .isDirectory)
    }

    func start() async {
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
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

        let clock = DisplayLinkClock(sink: periods)
        clock.setPreferredRate(Float(rate))
        clock.run(true)
        let error = startVariant()
        try? await Task.sleep(for: .seconds(seconds))
        clock.run(false)

        record.linkPeriodMedianNs = periods.median()
        record.debugger = record.debugger || Self.isTraced()
        write(frames: [], error: error)
        exit(0)
    }

    /// Starts the variant; the reason when it cannot.
    private func startVariant() -> String? {
        // MinatoBench.framework, the bench APKs and the layer's recorder come with ADR-0037 a) and b).
        "MinatoBench.framework and the bench APKs are not in this build yet (ADR-0037 a and b)"
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

    private func write(frames: [BenchFrame], error: String?) {
        try? BenchCSV.frames(frames).write(to: folder.appending(path: "frames.csv"), atomically: true, encoding: .utf8)
        try? BenchCSV.run(record).write(to: folder.appending(path: "run.csv"), atomically: true, encoding: .utf8)
        if let error {
            try? (error + "\n").write(to: folder.appending(path: "error"), atomically: true, encoding: .utf8)
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
