/// The interface with the scene (ADR-0037, step a): bench.conf in, scene.csv out, the same in both variants.
public enum BenchConf {
    /// ASCII `key=value` lines: run, rate, load and seconds, and the render scale (25 to 100 percent, M6's MetalFX
    /// plan, step 0) when it is not 100, the engine cap and the FPS unlock (ADR-0066 step 4) when they are not 0. The
    /// APK reads them from <internalDataPath>/bench.conf, the native build gets the text through MinatoBench's run
    /// call. `load` replaces the run's own load for a by-hand measure ("fill", the fill-heavy load of the scene).
    public static func text(for run: BenchRun, seconds: Int = BenchPlan.drawSeconds, scale: Int = 100,
                            load: String? = nil, cap: Int = 0, unlock: Int = 0) -> String {
        let configuration = run.configuration
        var lines = ["run=\(run)", "rate=\(configuration.rate)", "load=\(load ?? configuration.load.rawValue)",
                     "seconds=\(seconds)"]
        if scale != 100 { lines.append("scale=\(scale)") }
        if cap != 0 { lines.append("cap=\(cap)") }
        if unlock != 0 { lines.append("unlock=\(unlock)") }
        return BenchCSV.lines(lines)
    }
}

/// Test 2's done file (ADR-0042 c): "<frames> frames\n", which the script writes after scene.csv.
public enum BenchDone {
    /// The frames it counts; nil for anything else.
    public static func frames(_ text: String) -> Int? {
        let suffix = " frames\n"
        guard text.hasSuffix(suffix) else { return nil }
        let count = text.dropLast(suffix.count)
        guard !count.isEmpty, count.allSatisfy(\.isASCII), count.allSatisfy(\.isNumber) else { return nil }
        return Int(count)
    }
}

/// A drawable the layer recorded: frame k of a run is its k-th drawable.
public struct BenchDrawable: Equatable, Sendable {
    /// When nextDrawable returned.
    public var nextDrawableNs: Int64
    /// 0 for a drop: every presented handler has fired by the time the run is written.
    public var presentedNs: Int64

    public init(nextDrawableNs: Int64, presentedNs: Int64) {
        self.nextDrawableNs = nextDrawableNs
        self.presentedNs = presentedNs
    }
}

/// A row of scene.csv.
public struct BenchSceneFrame: Equatable, Sendable {
    public var index: Int
    public var tickNs: Int64
    public var cpuNs: Int64

    public init(index: Int, tickNs: Int64, cpuNs: Int64) {
        self.index = index
        self.tickNs = tickNs
        self.cpuNs = cpuNs
    }
}

extension BenchCSV {
    public static let sceneHeader = "index,tick_ns,cpu_ns"

    public static func scene(_ text: String) throws -> [BenchSceneFrame] {
        let rows = try parse(text)
        guard rows.first?.joined(separator: ",") == sceneHeader else { throw BenchCSVError.malformed("not a scene.csv") }
        return try rows.dropFirst().map { row in
            guard row.count == 3, let index = Int(row[0]), index >= 0, let tick = Int64(row[1]), let cpu = Int64(row[2])
            else { throw BenchCSVError.malformed(row.joined(separator: ",")) }
            return BenchSceneFrame(index: index, tickNs: tick, cpuNs: cpu)
        }
    }

    /// frames.csv's rows: drawable k with the scene's frame k, for the frames both have. Unequal
    /// counts make the run invalid (RunValidity).
    public static func join(drawables: [BenchDrawable], scene: [BenchSceneFrame]) -> [BenchFrame] {
        let frames = Dictionary(scene.map { ($0.index, $0) }, uniquingKeysWith: { first, _ in first })
        return drawables.enumerated().compactMap { index, drawable in
            frames[index].map {
                BenchFrame(index: index, tickNs: $0.tickNs, nextDrawableNs: drawable.nextDrawableNs,
                           presentedNs: drawable.presentedNs, cpuNs: $0.cpuNs)
            }
        }
    }
}
