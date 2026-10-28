import Testing

@testable import APKBench

/// ADR-0037, decisions 6 and 7: the series, interleaved A B B A, with a discarded pair per API.
@Suite("BenchPlan")
struct BenchPlanTests {
    static func run(_ id: String) -> BenchRun { BenchRun(id)! }

    static func result(_ run: BenchRun, valid: Bool = true, reasons: [String] = [], width: Int = 1290,
                       count: Int = 3) -> BenchResult {
        BenchResult(run: run, valid: valid, invalidReasons: reasons, drawableWidth: width, drawableHeight: 2796,
                    maxDrawableCount: count)
    }

    /// Runs the plan to its end; `outcome` decides each run's result.
    static func play(_ plan: BenchPlan, from start: [BenchResult] = [],
                     _ outcome: (BenchRun) -> BenchResult = { result($0) }) -> (runs: [BenchRun], end: BenchStep) {
        var results = start
        var runs: [BenchRun] = []
        while runs.count < 500 {
            guard case .run(let run) = plan.next(after: results) else { return (runs, plan.next(after: results)) }
            runs.append(run)
            results.append(outcome(run))
        }
        return (runs, .stop("the plan did not end"))
    }

    @Test func runIDsReadAndWrite() {
        let run = Self.run("gles-120-heavy-apk-03")
        #expect(run.configuration == BenchConfiguration(api: .gles, rate: 120, load: .heavy))
        #expect(run.variant == .apk)
        #expect(run.number == 3)
        #expect(!run.isWarmUp)
        #expect(run.description == "gles-120-heavy-apk-03")
        #expect(Self.run("vulkan-60-light-native-00").isWarmUp)
        for bad in ["gles-120-heavy-apk-3", "metal-120-heavy-apk-03", "gles-0-heavy-apk-03", "gles-120-heavy-apk",
                    "gles-120-medium-native-01", "gles-120-heavy-other-01", "gles-120-heavy-apk-03-x",
                    "vulkan-120-heavy-twin-03", "metal-120-heavy-native-03"] {
            #expect(BenchRun(bad) == nil, "\(bad)")
        }
    }

    /// ADR-0042, decision 5: test 2's runs begin with godot-; the twin is the native side, and Metal is the
    /// twin's alone.
    @Test func godotRunIDsReadAndWrite() {
        let run = Self.run("godot-vulkan-120-light-twin-03")
        #expect(run.configuration == BenchConfiguration(api: .vulkan, rate: 120, load: .light, scene: .godot))
        #expect(run.configuration.scene == .godot)
        #expect(run.variant == .twin)
        #expect(run.number == 3)
        #expect(run.description == "godot-vulkan-120-light-twin-03")
        #expect(Self.run("godot-metal-60-light-twin-00").isWarmUp)
        #expect(Self.run("godot-gles-120-heavy-apk-01").configuration.description == "godot-gles-120-heavy")
        for bad in ["godot-metal-120-light-apk-01", "godot-vulkan-120-light-native-01", "godot-gles-120-light-twin-1",
                    "godot-gles-120-light-twin", "godot-godot-gles-120-light-twin-01", "parity-gles-120-light-apk-01"] {
            #expect(BenchRun(bad) == nil, "\(bad)")
        }
    }

    @Test func theGodotSeriesPairsTheTwinWithTheAPKAndRunsMetalAlone() throws {
        let (runs, end) = Self.play(BenchPlan(scene: .godot))
        #expect(end == .done)
        #expect(runs.prefix(6).map(\.description) == [
            "godot-gles-120-heavy-twin-00", "godot-gles-120-heavy-apk-00",
            "godot-gles-60-light-twin-01", "godot-gles-60-light-apk-01", "godot-gles-60-light-apk-02",
            "godot-gles-60-light-twin-02",
        ])
        try #require(runs.count == 2 * (2 + 3 * 10) + (1 + 3 * 5))
        #expect(runs[64].description == "godot-metal-120-heavy-twin-00")
        #expect(runs[65].description == "godot-metal-60-light-twin-01")
        #expect(runs.last?.description == "godot-metal-120-heavy-twin-05")
        #expect(runs.allSatisfy { $0.configuration.scene == .godot && $0.variant != .native })
        #expect(runs.filter { $0.configuration.api == .metal }.allSatisfy { $0.variant == .twin })
    }

    /// The GLES twin draws on a CAEAGLLayer, which has no drawables to compare with the APK's layer.
    @Test func aRunWithoutLayerValuesIsNotComparedWithItsPair() {
        let (_, end) = Self.play(BenchPlan(scene: .godot, apis: [.gles])) { run in
            run.variant == .twin ? Self.result(run, width: 0, count: 0) : Self.result(run)
        }
        #expect(end == .done)
    }

    @Test func aSeriesBeginsWithTheDiscardedPairThenInterleaves() throws {
        let (runs, end) = Self.play(BenchPlan())
        #expect(end == .done)
        #expect(runs.prefix(12).map(\.description) == [
            "gles-120-heavy-native-00", "gles-120-heavy-apk-00",
            "gles-60-light-native-01", "gles-60-light-apk-01", "gles-60-light-apk-02", "gles-60-light-native-02",
            "gles-60-light-native-03", "gles-60-light-apk-03", "gles-60-light-apk-04", "gles-60-light-native-04",
            "gles-60-light-native-05", "gles-60-light-apk-05",
        ])
        try #require(runs.count == 2 * (2 + 3 * 10) + (1 + 3 * 5))
        #expect(runs[12].description == "gles-120-light-native-01")
        #expect(runs[32].description == "vulkan-120-heavy-native-00")
        #expect(runs[63].description == "vulkan-120-heavy-apk-05")
        #expect(runs[64].description == "gles31-120-heavy-apk-00")
        #expect(runs.last?.description == "gles31-120-heavy-apk-05")
        for api in BenchScene.parity.apis {
            for configuration in BenchConfiguration.series(of: api) {
                for variant in BenchScene.parity.variants(of: api) {
                    let counted = runs.filter { $0.configuration == configuration && $0.variant == variant && !$0.isWarmUp }
                    #expect(counted.count == BenchPlan.validRunsPerVariant, "\(configuration) \(variant)")
                }
            }
        }
    }

    @Test func anInvalidRunIsRepeatedRightAway() {
        let start = [Self.result(Self.run("gles-120-heavy-native-00")), Self.result(Self.run("gles-120-heavy-apk-00")),
                     Self.result(Self.run("gles-60-light-native-01")),
                     Self.result(Self.run("gles-60-light-apk-01"), valid: false, reasons: [RunValidity.focusLoss])]
        #expect(BenchPlan().next(after: start) == .run(Self.run("gles-60-light-apk-02")))
        let repeated = start + [Self.result(Self.run("gles-60-light-apk-02"))]
        // The interleaving counts valid runs: the second B comes next.
        #expect(BenchPlan().next(after: repeated) == .run(Self.run("gles-60-light-apk-03")))
    }

    @Test func theResultsMayComeInAnyOrder() {
        let results = [Self.result(Self.run("gles-60-light-native-01")), Self.result(Self.run("gles-120-heavy-apk-00")),
                       Self.result(Self.run("gles-120-heavy-native-00"))]
        #expect(BenchPlan().next(after: results) == .run(Self.run("gles-60-light-apk-01")))
    }

    @Test func aWarmUpRunCountsWhateverItsValidity() {
        let results = [Self.result(Self.run("gles-120-heavy-native-00"), valid: false, reasons: [RunValidity.debugger])]
        #expect(BenchPlan().next(after: results) == .run(Self.run("gles-120-heavy-apk-00")))
    }

    @Test func threeInvalidRunsInARowStopTheSeries() {
        var invalid = 0
        let (runs, end) = Self.play(BenchPlan()) { run in
            guard run.description.hasPrefix("gles-60-light-apk") else { return Self.result(run) }
            invalid += 1
            return Self.result(run, valid: false, reasons: [RunValidity.thermalAboveFair, RunValidity.linkPeriod])
        }
        #expect(invalid == BenchPlan.invalidRunsInARow)
        #expect(runs.last?.description == "gles-60-light-apk-03")
        guard case .stop(let reason) = end else { Issue.record("no stop: \(end)"); return }
        #expect(reason.contains("gles-60-light"))
        #expect(reason.contains(RunValidity.thermalAboveFair))
        #expect(reason.contains(RunValidity.linkPeriod))
    }

    @Test func twoInvalidRunsThenAValidOneGoOn() {
        var invalid = 0
        let (_, end) = Self.play(BenchPlan()) { run in
            guard run.description.hasPrefix("gles-60-light-apk"), invalid < 2 else { return Self.result(run) }
            invalid += 1
            return Self.result(run, valid: false, reasons: [RunValidity.focusLoss])
        }
        #expect(end == .done)
    }

    @Test func aPairMismatchStopsAtOnce() {
        for mismatch in [Self.result(Self.run("gles-60-light-apk-01"), width: 1179),
                         Self.result(Self.run("gles-60-light-apk-01"), count: 2)] {
            let results = [Self.result(Self.run("gles-120-heavy-native-00")),
                           Self.result(Self.run("gles-120-heavy-apk-00")),
                           Self.result(Self.run("gles-60-light-native-01")), mismatch]
            guard case .stop(let reason) = BenchPlan().next(after: results) else {
                Issue.record("no stop for \(mismatch)")
                continue
            }
            #expect(reason.contains("gles-60-light"))
            #expect(reason.contains("pair"))
        }
    }

    @Test func theWarmUpPairIsNotComparedWithTheSeries() {
        let results = [Self.result(Self.run("gles-120-heavy-native-00"), width: 1179),
                       Self.result(Self.run("gles-120-heavy-apk-00"), count: 2)]
        #expect(BenchPlan().next(after: results) == .run(Self.run("gles-60-light-native-01")))
    }

    @Test func moreRunsAddFivePerVariant() {
        let (runs, _) = Self.play(BenchPlan())
        let finished = runs.map { Self.result($0) }
        let noisy = BenchConfiguration("gles-120-heavy")!
        let (extra, end) = Self.play(BenchPlan(more: [noisy]), from: finished)
        #expect(end == .done)
        #expect(extra.map(\.description) == [
            "gles-120-heavy-apk-06", "gles-120-heavy-native-06", "gles-120-heavy-native-07", "gles-120-heavy-apk-07",
            "gles-120-heavy-apk-08", "gles-120-heavy-native-08", "gles-120-heavy-native-09", "gles-120-heavy-apk-09",
            "gles-120-heavy-apk-10", "gles-120-heavy-native-10",
        ])
    }

    @Test func oneAPIOnly() {
        let (runs, end) = Self.play(BenchPlan(apis: [.vulkan]))
        #expect(end == .done)
        #expect(runs.first?.description == "vulkan-120-heavy-native-00")
        #expect(runs.count == 32)
    }

    /// ADR-0066 step 4: one configuration of an API's series, after the API's discarded pair, as the by-hand cap
    /// and unlock runs need.
    @Test func oneConfigurationOnly() {
        let only = BenchConfiguration("vulkan-120-light")!
        let (runs, end) = Self.play(BenchPlan(only: only))
        #expect(end == .done)
        #expect(runs.prefix(6).map(\.description) == [
            "vulkan-120-heavy-native-00", "vulkan-120-heavy-apk-00", "vulkan-120-light-native-01",
            "vulkan-120-light-apk-01", "vulkan-120-light-apk-02", "vulkan-120-light-native-02",
        ])
        #expect(runs.count == 2 + 10)
        #expect(runs.dropFirst(2).allSatisfy { $0.configuration == only })
        let (alone, _) = Self.play(BenchPlan(only: BenchConfiguration("gles31-120-light")!))
        #expect(alone.map(\.description).prefix(2) == ["gles31-120-heavy-apk-00", "gles31-120-light-apk-01"])
        #expect(alone.count == 1 + 5)
        let (more, _) = Self.play(BenchPlan(more: [only], only: only))
        #expect(more.count == 2 + 20)
    }

    /// Only a configuration of the scene's series runs alone: another rate or the other scene's has no verdict.
    @Test func onlyTakesASeriesConfiguration() {
        #expect(BenchPlan.seriesConfiguration("gles-120-light", scene: .parity) != nil)
        #expect(BenchPlan.seriesConfiguration("godot-metal-60-light", scene: .godot) != nil)
        for (bad, scene) in [("gles-90-light", BenchScene.parity), ("gles-60-heavy", .parity),
                             ("godot-gles-120-light", .parity), ("gles-120-light", .godot), ("gles-120", .parity)] {
            #expect(BenchPlan.seriesConfiguration(bad, scene: scene) == nil, "\(bad)")
        }
    }

    /// ADR-0044 d2): bench-gles31, bench-gles's scene on ANGLE's Vulkan backend, runs its APK alone: there is no
    /// native ES 3.1 build, and its numbers are reported beside bench-gles's, not judged (ADR-0044 decision 5).
    @Test func theES31TwinRunsItsAPKAlone() {
        let (runs, end) = Self.play(BenchPlan(apis: [.gles31]))
        #expect(end == .done)
        #expect(runs.prefix(3).map(\.description) == [
            "gles31-120-heavy-apk-00", "gles31-60-light-apk-01", "gles31-60-light-apk-02",
        ])
        #expect(runs.count == 1 + 3 * 5)
        #expect(runs.allSatisfy { $0.configuration.api == .gles31 && $0.variant == .apk })
        let run = BenchRun("gles31-120-heavy-apk-03")
        #expect(run?.configuration == BenchConfiguration(api: .gles31, rate: 120, load: .heavy))
        for bad in ["gles31-120-heavy-native-03", "gles31-120-heavy-twin-03", "godot-gles31-120-heavy-apk-03"] {
            #expect(BenchRun(bad) == nil, "\(bad)")
        }
    }
}
