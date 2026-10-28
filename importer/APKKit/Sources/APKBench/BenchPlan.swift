/// The parity benchmark's series (ADR-0037, decisions 4, 6 and 7) and test 2's (ADR-0042, decision 5): which run
/// comes next.
public enum BenchVariant: String, Sendable, CaseIterable {
    /// A: the scene built for iOS, in MinatoBench.framework.
    case native
    /// B: the scene as an APK in Minato.
    case apk
    /// Test 2's A: Godot's iOS template in the benchmark build, MinatoGodot.framework (ADR-0042, decision 3).
    case twin
}

public enum BenchAPI: String, Sendable, CaseIterable {
    case gles
    case vulkan
    /// bench-gles's scene under an ES 3.1 manifest, which Minato starts on ANGLE's Vulkan backend (ADR-0044 d2)):
    /// its APK runs alone, reported beside bench-gles and not judged (ADR-0044 decision 5).
    case gles31
    /// The twin's Metal driver, Godot's default on iOS: reported against the Vulkan APK, not judged (ADR-0042).
    case metal
}

/// What a run draws: ADR-0037's parity scene, or test 2's Godot project (ADR-0042), whose IDs begin with godot-.
public enum BenchScene: String, Sendable, CaseIterable {
    case parity
    case godot

    public var apis: [BenchAPI] { self == .parity ? [.gles, .vulkan, .gles31] : [.gles, .vulkan, .metal] }

    /// The variants a configuration compares, A first, then B; the Metal twin and the ES 3.1 APK run alone.
    public func variants(of api: BenchAPI) -> [BenchVariant] {
        switch (self, api) {
        case (.parity, .gles), (.parity, .vulkan): [.native, .apk]
        case (.parity, .gles31): [.apk]
        case (.parity, .metal): []
        case (.godot, .gles), (.godot, .vulkan): [.twin, .apk]
        case (.godot, .metal): [.twin]
        case (.godot, .gles31): []
        }
    }
}

public enum BenchLoad: String, Sendable, CaseIterable {
    case light
    case heavy
}

/// One API, rate and load: the runs of both variants that a verdict compares.
public struct BenchConfiguration: Hashable, Sendable, CustomStringConvertible {
    public var api: BenchAPI
    public var rate: Int
    public var load: BenchLoad
    public var scene: BenchScene

    public init(api: BenchAPI, rate: Int, load: BenchLoad, scene: BenchScene = .parity) {
        self.api = api
        self.rate = rate
        self.load = load
        self.scene = scene
    }

    /// `gles-120-heavy`, or test 2's `godot-gles-120-heavy`.
    public init?(_ description: String) {
        var parts = description.split(separator: "-", omittingEmptySubsequences: false).map(String.init)
        let scene: BenchScene = parts.first == BenchScene.godot.rawValue ? .godot : .parity
        if scene == .godot { parts.removeFirst() }
        guard parts.count == 3, let api = BenchAPI(rawValue: parts[0]), scene.apis.contains(api),
            let rate = Int(parts[1]), rate > 0, let load = BenchLoad(rawValue: parts[2])
        else { return nil }
        self.init(api: api, rate: rate, load: load, scene: scene)
    }

    public var description: String {
        "\(scene == .godot ? "godot-" : "")\(api.rawValue)-\(rate)-\(load.rawValue)"
    }

    /// Decision 7, per API: a light load at 60 and 120 Hz and a heavy load at 120 Hz.
    public static func series(of api: BenchAPI, scene: BenchScene = .parity) -> [BenchConfiguration] {
        [.init(api: api, rate: 60, load: .light, scene: scene), .init(api: api, rate: 120, load: .light, scene: scene),
         .init(api: api, rate: 120, load: .heavy, scene: scene)]
    }

    /// The discarded pair that fills Metal's shader cache before an API's series.
    public static func warmUp(of api: BenchAPI, scene: BenchScene = .parity) -> BenchConfiguration {
        .init(api: api, rate: 120, load: .heavy, scene: scene)
    }
}

/// One process with one variant: `gles-120-heavy-apk-03`, or test 2's `godot-gles-120-heavy-twin-03`. Number 0 is
/// the discarded warm-up run.
public struct BenchRun: Hashable, Sendable, CustomStringConvertible {
    public var configuration: BenchConfiguration
    public var variant: BenchVariant
    public var number: Int

    public init(configuration: BenchConfiguration, variant: BenchVariant, number: Int) {
        self.configuration = configuration
        self.variant = variant
        self.number = number
    }

    public init?(_ description: String) {
        let parts = description.split(separator: "-", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 5, let configuration = BenchConfiguration(parts.dropLast(2).joined(separator: "-")),
            let variant = BenchVariant(rawValue: parts[parts.count - 2]),
            configuration.scene.variants(of: configuration.api).contains(variant), parts[parts.count - 1].count == 2,
            let number = Int(parts[parts.count - 1]), number >= 0
        else { return nil }
        self.init(configuration: configuration, variant: variant, number: number)
    }

    public var isWarmUp: Bool { number == 0 }

    public var description: String {
        "\(configuration)-\(variant.rawValue)-\(number < 10 ? "0" : "")\(number)"
    }
}

/// What the plan needs to know of a finished run, from its run.csv.
public struct BenchResult: Hashable, Sendable {
    public var run: BenchRun
    public var valid: Bool
    public var invalidReasons: [String]
    public var drawableWidth: Int
    public var drawableHeight: Int
    public var maxDrawableCount: Int

    public init(run: BenchRun, valid: Bool, invalidReasons: [String] = [], drawableWidth: Int,
                drawableHeight: Int, maxDrawableCount: Int) {
        self.run = run
        self.valid = valid
        self.invalidReasons = invalidReasons
        self.drawableWidth = drawableWidth
        self.drawableHeight = drawableHeight
        self.maxDrawableCount = maxDrawableCount
    }
}

public enum BenchStep: Equatable, Sendable {
    case run(BenchRun)
    case done
    /// Something a repeat would not fix: the series stops, with the reason.
    case stop(String)
}

public struct BenchPlan: Sendable {
    /// Decision 4: the scene draws this long from its first frame.
    public static let drawSeconds = 52
    /// Decision 3: the longest wait for a nominal thermal state before a run.
    public static let thermalWaitSeconds = 600
    /// Decision 7: valid runs per variant and configuration, and what the noise rule adds.
    public static let validRunsPerVariant = 5
    /// Invalid runs in a row of one configuration after which the series stops.
    public static let invalidRunsInARow = 3

    public var scene: BenchScene
    public var apis: [BenchAPI]
    /// Configurations whose native runs spread more than 5 %: 5 more runs per variant (decision 8).
    public var more: Set<BenchConfiguration>
    /// One configuration of its API's series alone, after the API's discarded pair (ADR-0066 step 4's by-hand runs).
    public var only: BenchConfiguration?

    /// Every API of the scene unless `apis` names some, or `only`'s API.
    public init(scene: BenchScene = .parity, apis: [BenchAPI]? = nil, more: Set<BenchConfiguration> = [],
                only: BenchConfiguration? = nil) {
        self.scene = only?.scene ?? scene
        self.apis = only.map { [$0.api] } ?? apis ?? self.scene.apis
        self.more = more
        self.only = only
    }

    /// `description` when it names a configuration of `scene`'s series.
    public static func seriesConfiguration(_ description: String, scene: BenchScene) -> BenchConfiguration? {
        guard let configuration = BenchConfiguration(description), configuration.scene == scene,
            BenchConfiguration.series(of: configuration.api, scene: scene).contains(configuration)
        else { return nil }
        return configuration
    }

    /// The next run, given the results so far in any order.
    public func next(after results: [BenchResult]) -> BenchStep {
        for api in apis {
            let variants = scene.variants(of: api)
            for variant in variants {
                let warmUp = BenchRun(configuration: .warmUp(of: api, scene: scene), variant: variant, number: 0)
                if !results.contains(where: { $0.run == warmUp }) { return .run(warmUp) }
            }
            for configuration in only.map({ [$0] }) ?? BenchConfiguration.series(of: api, scene: scene) {
                let own = results.filter { $0.run.configuration == configuration && !$0.run.isWarmUp }
                if let mismatch = Self.pairMismatch(own) { return .stop("\(configuration): \(mismatch)") }
                let valid = own.filter(\.valid).count
                let wanted = variants.count * Self.validRunsPerVariant * (more.contains(configuration) ? 2 : 1)
                guard valid < wanted else { continue }
                // An invalid run is repeated right away, so the valid runs follow A B B A.
                let variant = variants.count == 1 || [0, 3].contains(valid % 4) ? variants[0] : variants[1]
                let runs = own.filter { $0.run.variant == variant }.sorted { $0.run.number < $1.run.number }
                let invalid = runs.reversed().prefix { !$0.valid }
                if invalid.count >= Self.invalidRunsInARow {
                    var reasons: [String] = []
                    for reason in invalid.reversed().flatMap(\.invalidReasons) where !reasons.contains(reason) {
                        reasons.append(reason)
                    }
                    return .stop("\(configuration): \(invalid.count) invalid \(variant.rawValue) runs in a row: "
                        + reasons.joined(separator: "; "))
                }
                return .run(BenchRun(configuration: configuration, variant: variant,
                                     number: (runs.last?.run.number ?? 0) + 1))
            }
        }
        return .done
    }

    /// Decision 6: the layer's size and drawable count as in the other variant. A mismatch is
    /// systematic, so repeating would not help. A run without layer values, the GLES twin's CAEAGLLayer
    /// (ADR-0042, decision 4), has nothing to compare.
    static func pairMismatch(_ results: [BenchResult]) -> String? {
        guard let configuration = results.first?.run.configuration else { return nil }
        let variants = configuration.scene.variants(of: configuration.api)
        guard variants.count == 2 else { return nil }
        func layers(_ variant: BenchVariant) -> Set<String> {
            Set(results.filter { $0.valid && $0.run.variant == variant && $0.maxDrawableCount > 0 }.map {
                "\($0.drawableWidth)x\($0.drawableHeight)/\($0.maxDrawableCount)"
            })
        }
        let first = layers(variants[0]), second = layers(variants[1])
        guard !first.isEmpty, !second.isEmpty, first != second else { return nil }
        return "the pair differs in layer size or drawable count: \(variants[0].rawValue) "
            + "\(first.sorted().joined(separator: " ")), "
            + "\(variants[1].rawValue) \(second.sorted().joined(separator: " "))"
    }
}
