import Testing

@testable import APKBench

/// ADR-0037, decision 6: the rules of one run.
@Suite("RunValidity")
struct RunValidityTests {
    static func record(rate: Int = 120) -> BenchRunRecord {
        var record = BenchRunRecord(run: BenchRun("gles-\(rate)-light-native-01")!)
        record.requestedRate = Float(rate)
        record.linkPeriodMedianNs = 1_000_000_000 / Int64(rate)
        record.sceneFrames = 52 * rate
        record.drawables = 52 * rate
        return record
    }

    @Test func aRunInGoodConditionsIsValid() {
        var record = Self.record()
        record.thermalChanges = [.init(timeNs: 1, state: .fair), .init(timeNs: 2, state: .nominal)]
        #expect(RunValidity.reasons(for: record) == [])
    }

    @Test func lowPowerMode() {
        var record = Self.record()
        record.lowPower = true
        #expect(RunValidity.reasons(for: record) == [RunValidity.lowPower])
    }

    @Test func theStartMustBeNominal() {
        var record = Self.record()
        record.thermalStart = .fair
        #expect(RunValidity.reasons(for: record) == [RunValidity.thermalStart])
    }

    @Test func neverAboveFair() {
        for state in [BenchThermal.serious, .critical] {
            var record = Self.record()
            record.thermalChanges = [.init(timeNs: 1, state: state), .init(timeNs: 2, state: .nominal)]
            #expect(RunValidity.reasons(for: record) == [RunValidity.thermalAboveFair], "\(state)")
        }
    }

    @Test func theDisplayLinkWithinOnePercent() {
        for rate in [60, 120] {
            let period = 1_000_000_000.0 / Double(rate)
            for (factor, valid) in [(1.0099, true), (0.9901, true), (1.0101, false), (0.9899, false), (0.0, false)] {
                var record = Self.record(rate: rate)
                record.linkPeriodMedianNs = Int64(period * factor)
                #expect(RunValidity.reasons(for: record) == (valid ? [] : [RunValidity.linkPeriod]), "\(rate) \(factor)")
            }
        }
    }

    /// ADR-0042: an APK that asks for no rate runs at the panel's rate and is judged by its frame times; the same
    /// row with a rate asked, and the twin, must match their rate.
    @Test func anApkThatAsksNoRateNeedsNoLinkPeriod() {
        func record(_ run: String, requested: Float) -> BenchRunRecord {
            var record = BenchRunRecord(run: BenchRun(run)!)
            record.requestedRate = requested
            record.linkPeriodMedianNs = 8_335_167
            record.sceneFrames = 52 * 60
            record.drawables = 52 * 60
            return record
        }
        #expect(RunValidity.reasons(for: record("gles-60-light-apk-01", requested: 0)) == [])
        #expect(RunValidity.reasons(for: record("gles-60-light-apk-01", requested: 60)) == [RunValidity.linkPeriod])
        #expect(RunValidity.reasons(for: record("gles-60-light-native-01", requested: 0)) == [RunValidity.linkPeriod])
    }

    @Test func noDebugger() {
        var record = Self.record()
        record.debugger = true
        #expect(RunValidity.reasons(for: record) == [RunValidity.debugger])
    }

    @Test func noLossOfFocus() {
        var record = Self.record()
        record.focusLosses = [5]
        #expect(RunValidity.reasons(for: record) == [RunValidity.focusLoss])
    }

    /// A drawable taken before frame 0 would shift every frame by one: the counts must agree.
    @Test func theSceneAndTheRecorderCountAlike() {
        for (frames, drawables) in [(6240, 6241), (6241, 6240), (0, 6240), (0, 0)] {
            var record = Self.record()
            record.sceneFrames = frames
            record.drawables = drawables
            #expect(RunValidity.reasons(for: record) == ["frames \(frames), drawables \(drawables)"])
        }
    }

    /// ADR-0042, decision 4: test 2's script ends with its done file, and the GLES twin's frames are the
    /// script's alone.
    @Test func godotRunsEndWithTheirDoneFile() {
        func record(_ run: String, done: Int, drawables: Int) -> BenchRunRecord {
            var record = BenchRunRecord(run: BenchRun(run)!)
            record.linkPeriodMedianNs = 1_000_000_000 / 120
            record.sceneFrames = 6240
            record.doneFrames = done
            record.drawables = drawables
            return record
        }
        #expect(RunValidity.reasons(for: record("godot-vulkan-120-light-twin-01", done: 6240, drawables: 6240)) == [])
        #expect(RunValidity.reasons(for: record("godot-gles-120-light-twin-01", done: 6240, drawables: 0)) == [])
        #expect(RunValidity.reasons(for: record("godot-gles-120-light-apk-01", done: 6240, drawables: 0))
            == ["frames 6240, drawables 0"])
        #expect(RunValidity.reasons(for: record("godot-metal-120-light-twin-01", done: 6239, drawables: 6240))
            == ["done 6239, frames 6240"])
        var empty = record("godot-gles-120-light-twin-01", done: 0, drawables: 0)
        empty.sceneFrames = 0
        #expect(RunValidity.reasons(for: empty) == ["frames 0, drawables 0"])
        // ADR-0037's scene writes no done file.
        #expect(RunValidity.reasons(for: Self.record()) == [])
    }

    @Test func everyReasonCounts() {
        var record = Self.record()
        record.lowPower = true
        record.thermalStart = .serious
        record.linkPeriodMedianNs = 16_666_667
        record.debugger = true
        record.focusLosses = [5]
        record.drawables += 1
        #expect(RunValidity.reasons(for: record) == [RunValidity.lowPower, RunValidity.thermalStart,
                                                     RunValidity.thermalAboveFair, RunValidity.linkPeriod,
                                                     RunValidity.debugger, RunValidity.focusLoss,
                                                     "frames 6240, drawables 6241"])
    }
}
