/// The parity benchmark's series (ADR-0037, decisions 4, 6 and 7): which run comes next.
public enum BenchVariant: String, Sendable, CaseIterable {
    /// A: the scene built for iOS, in MinatoBench.framework.
    case native
    /// B: the scene as an APK in Minato.
    case apk
}

public enum BenchAPI: String, Sendable, CaseIterable {
    case gles
    case vulkan
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

    public init(api: BenchAPI, rate: Int, load: BenchLoad) {
        self.api = api
        self.rate = rate
        self.load = load
    }

    /// `gles-120-heavy`.
    public init?(_ description: String) {
        let parts = description.split(separator: "-", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 3, let api = BenchAPI(rawValue: parts[0]), let rate = Int(parts[1]), rate > 0,
            let load = BenchLoad(rawValue: parts[2])
        else { return nil }
        self.init(api: api, rate: rate, load: load)
    }

    public var description: String { "\(api.rawValue)-\(rate)-\(load.rawValue)" }

    /// Decision 7, per API: a light load at 60 and 120 Hz and a heavy load at 120 Hz.
    public static func series(of api: BenchAPI) -> [BenchConfiguration] {
        [.init(api: api, rate: 60, load: .light), .init(api: api, rate: 120, load: .light),
         .init(api: api, rate: 120, load: .heavy)]
    }

    /// The discarded pair that fills Metal's shader cache before an API's series.
    public static func warmUp(of api: BenchAPI) -> BenchConfiguration { .init(api: api, rate: 120, load: .heavy) }
}

/// One process with one variant: `gles-120-heavy-apk-03`. Number 0 is the discarded warm-up run.
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
        guard parts.count == 5, let configuration = BenchConfiguration(parts[0...2].joined(separator: "-")),
            let variant = BenchVariant(rawValue: parts[3]), parts[4].count == 2, let number = Int(parts[4]),
            number >= 0
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

    public var apis: [BenchAPI]
    /// Configurations whose native runs spread more than 5 %: 5 more runs per variant (decision 8).
    public var more: Set<BenchConfiguration>

    public init(apis: [BenchAPI] = BenchAPI.allCases, more: Set<BenchConfiguration> = []) {
        self.apis = apis
        self.more = more
    }

    /// The next run, given the results so far in any order.
    public func next(after results: [BenchResult]) -> BenchStep {
        for api in apis {
            for variant in BenchVariant.allCases {
                let warmUp = BenchRun(configuration: .warmUp(of: api), variant: variant, number: 0)
                if !results.contains(where: { $0.run == warmUp }) { return .run(warmUp) }
            }
            for configuration in BenchConfiguration.series(of: api) {
                let own = results.filter { $0.run.configuration == configuration && !$0.run.isWarmUp }
                if let mismatch = Self.pairMismatch(own) { return .stop("\(configuration): \(mismatch)") }
                let valid = own.filter(\.valid).count
                let wanted = 2 * Self.validRunsPerVariant * (more.contains(configuration) ? 2 : 1)
                guard valid < wanted else { continue }
                // An invalid run is repeated right away, so the valid runs follow A B B A.
                let variant: BenchVariant = [0, 3].contains(valid % 4) ? .native : .apk
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
    /// systematic, so repeating would not help.
    static func pairMismatch(_ results: [BenchResult]) -> String? {
        func layers(_ variant: BenchVariant) -> Set<String> {
            Set(results.filter { $0.valid && $0.run.variant == variant }.map {
                "\($0.drawableWidth)x\($0.drawableHeight)/\($0.maxDrawableCount)"
            })
        }
        let native = layers(.native), apk = layers(.apk)
        guard !native.isEmpty, !apk.isEmpty, native != apk else { return nil }
        return "the pair differs in layer size or drawable count: native \(native.sorted().joined(separator: " ")), "
            + "apk \(apk.sorted().joined(separator: " "))"
    }
}
