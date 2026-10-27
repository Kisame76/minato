import Foundation
import Testing

@testable import APKBench

/// ADR-0037, decision 5: frames.csv and run.csv, which tools/bench-summary.py reads.
@Suite("BenchCSV")
struct BenchCSVTests {
    static func record() -> BenchRunRecord {
        var record = BenchRunRecord(run: BenchRun("vulkan-120-heavy-apk-02")!)
        record.meshes = 400
        record.requestedRate = 120
        record.maxFPS = 120
        record.linkPeriodMedianNs = 8_333_333
        record.thermalStart = .nominal
        record.thermalChanges = [.init(timeNs: 5_000_000_000, state: .fair), .init(timeNs: 9_000_000_000, state: .nominal)]
        record.batteryState = "unplugged"
        record.batteryLevel = 0.85
        record.qos = "default"
        record.pixelFormat = "bgra8Unorm"
        record.drawableWidth = 1290
        record.drawableHeight = 2796
        record.maxDrawableCount = 3
        record.commit = "8ea8ae5"
        record.configuration = "Release"
        record.startNs = 1_000_000_000
        record.firstPresentNs = 1_250_000_000
        record.sceneFrames = 6240
        record.drawables = 6240
        return record
    }

    @Test func framesAreOneRowEach() {
        let text = BenchCSV.frames([
            BenchFrame(index: 0, tickNs: 100, nextDrawableNs: 110, presentedNs: 8_433_433, cpuNs: 2_000_000),
            BenchFrame(index: 1, tickNs: 8_333_433, nextDrawableNs: 8_333_500, presentedNs: 0, cpuNs: 1_900_000),
        ])
        #expect(text == """
            index,tick_ns,next_drawable_ns,presented_ns,cpu_ns
            0,100,110,8433433,2000000
            1,8333433,8333500,0,1900000

            """)
    }

    /// The row under the header.
    static func row(_ record: BenchRunRecord) throws -> Substring {
        let lines = BenchCSV.run(record).split(separator: "\n")
        try #require(lines.count == 2)
        return lines[1]
    }

    @Test func aRunIsTheHeaderAndOneRow() throws {
        let lines = BenchCSV.run(Self.record()).split(separator: "\n", omittingEmptySubsequences: false)
        try #require(lines.count == 3)
        #expect(lines[0] == BenchCSV.runHeader)
        #expect(lines[1] == """
            vulkan-120-heavy-apk-02,apk,vulkan,120,heavy,400,120.0,120,8333333,nominal,nominal,\
            5000000000=fair|9000000000=nominal,0,unplugged,0.85,default,bgra8Unorm,1290,2796,3,8ea8ae5,Release,0,\
            ,1000000000,1250000000,0,,1,
            """)
        #expect(lines[2] == "")
    }

    /// ADR-0044 d2): bench-gles31's run.csv names its own API, though it draws bench-gles's scene (App's review).
    @Test func anES31RunNamesItsOwnAPI() throws {
        let record = BenchRunRecord(run: BenchRun("gles31-120-heavy-apk-02")!)
        #expect(try Self.row(record).hasPrefix("gles31-120-heavy-apk-02,apk,gles31,120,heavy,"))
    }

    @Test func anInvalidRunCarriesItsReasons() throws {
        var record = Self.record()
        record.debugger = true
        record.focusLosses = [2_000_000_000, 3_000_000_000]
        let row = try Self.row(record)
        #expect(row.hasSuffix(",1,2000000000|3000000000,1000000000,1250000000,0,,0,debugger|focus_loss"))
        // Godot's APK: the drawables before frame 0, and those after the done file.
        var godot = BenchRunRecord(run: BenchRun("godot-vulkan-60-light-apk-01")!)
        godot.drawablesBeforeFrame0 = 3
        godot.drawablesAfterDone = 1
        #expect(try Self.row(godot).contains(",0,0,3,1,0,"))
    }

    @Test func aReasonWithACommaIsQuoted() throws {
        var record = Self.record()
        record.drawables = 6241
        #expect(try Self.row(record).hasSuffix(",0,\"frames 6240, drawables 6241\""))
        #expect(try BenchCSV.result(fromRun: BenchCSV.run(record)).invalidReasons == ["frames 6240, drawables 6241"])
    }

    @Test func textWithCommasOrQuotesIsQuoted() throws {
        var record = Self.record()
        record.configuration = "Release, \"bench\""
        let row = try Self.row(record)
        #expect(row.contains(",8ea8ae5,\"Release, \"\"bench\"\"\",0,"))
    }

    @Test func thePlanReadsARunBack() throws {
        var record = Self.record()
        record.configuration = "Release, \"bench\""
        record.lowPower = true
        let result = try BenchCSV.result(fromRun: BenchCSV.run(record))
        #expect(result == BenchResult(run: record.run, valid: false, invalidReasons: [RunValidity.lowPower],
                                      drawableWidth: 1290, drawableHeight: 2796, maxDrawableCount: 3))
        #expect(try BenchCSV.result(fromRun: BenchCSV.run(Self.record())).valid)
    }

    @Test func aBrokenRunFileIsAnError() {
        let good = BenchCSV.run(Self.record())
        for bad in ["", BenchCSV.runHeader, good.replacingOccurrences(of: "vulkan-120-heavy-apk-02", with: "x"),
                    good.replacingOccurrences(of: ",1290,", with: ",wide,"), "a,b\n1,2\n"] {
            #expect(throws: BenchCSVError.self, "\(bad)") { try BenchCSV.result(fromRun: bad) }
        }
    }
}
