import Foundation
import Testing

@testable import APKBench

/// ADR-0041 d), the App's part: drawables.csv and session.csv of a recorded session (-recordSession).
@Suite("BenchSession")
struct BenchSessionTests {
    static func record() -> BenchSessionRecord {
        var record = BenchSessionRecord(package: "io.github.kisame76.minato.test.present")
        record.maxFPS = 120
        record.linkPeriodMedianNs = 8_333_333
        record.thermalStart = .nominal
        record.thermalChanges = [.init(timeNs: 5_000_000_000, state: .fair)]
        record.batteryState = "unplugged"
        record.batteryLevel = 0.85
        record.commit = "1cc582e"
        record.configuration = "Release"
        record.qos = "default"
        record.pixelFormat = "bgra8Unorm"
        record.drawableWidth = 1290
        record.drawableHeight = 2796
        record.maxDrawableCount = 3
        record.exitStatus = 0
        return record
    }

    @Test func drawablesAreOneRowEachInOrder() {
        let text = BenchCSV.drawables([
            BenchDrawable(nextDrawableNs: 110, presentedNs: 8_433_433),
            BenchDrawable(nextDrawableNs: 8_333_500, presentedNs: 0),
            BenchDrawable(nextDrawableNs: 16_666_900, presentedNs: 33_333_333, tickTimed: true),
        ])
        #expect(text == """
            frame,next_drawable_ns,presented_ns,tick_timed
            0,110,8433433,0
            1,8333500,0,0
            2,16666900,33333333,1

            """)
        #expect(BenchCSV.drawables([]) == "frame,next_drawable_ns,presented_ns,tick_timed\n")
    }

    @Test func aSessionIsTheHeaderAndOneRow() throws {
        let lines = BenchCSV.session(Self.record()).split(separator: "\n", omittingEmptySubsequences: false)
        try #require(lines.count == 3)
        #expect(lines[0] == """
            package,max_fps,link_period_median_ns,thermal_start,thermal_end,thermal_changes,low_power,\
            battery_state,battery_level,commit,configuration,debugger,qos,pixel_format,drawable_width,\
            drawable_height,max_drawable_count,lost,unanswered,exit_status
            """)
        #expect(lines[1] == """
            io.github.kisame76.minato.test.present,120,8333333,nominal,fair,5000000000=fair,0,unplugged,0.85,\
            1cc582e,Release,0,default,bgra8Unorm,1290,2796,3,0,0,0
            """)
        #expect(lines[2] == "")
    }

    @Test func aSessionWithoutAnEndHasNoExitStatus() throws {
        var record = Self.record()
        record.exitStatus = nil
        record.lost = 4
        record.unanswered = 2
        record.lowPower = true
        record.debugger = true
        record.commit = "1cc582e-dirty, \"local\""
        let row = try #require(BenchCSV.session(record).split(separator: "\n").last)
        #expect(row.hasSuffix(",1,unplugged,0.85,\"1cc582e-dirty, \"\"local\"\"\",Release,1,default,bgra8Unorm,1290,2796,3,4,2,"))
    }

    @Test func theLayersValuesComeFromItsRecording() {
        var record = BenchSessionRecord(package: "p")
        record.take(BenchRecording(qos: "userInteractive", pixelFormat: 80, drawableWidth: 1320, drawableHeight: 2868,
                                   maxDrawableCount: 3, drawables: [BenchDrawable(nextDrawableNs: 1, presentedNs: 2)],
                                   lost: 5))
        #expect(record.qos == "userInteractive")
        #expect(record.pixelFormat == "bgra8Unorm")
        #expect(record.drawableWidth == 1320)
        #expect(record.drawableHeight == 2868)
        #expect(record.maxDrawableCount == 3)
        #expect(record.lost == 5)
    }

    /// Drawable k is frame k only while nothing but the guest's presents takes drawables from the layer.
    @Test func everyDrawableIsAPresentOfTheGuest() {
        #expect(BenchSessionRecord.drawableNote(drawables: 1300, lost: 0, presents: 1300) == nil)
        #expect(BenchSessionRecord.drawableNote(drawables: 7200, lost: 100, presents: 7300) == nil)
        #expect(BenchSessionRecord.drawableNote(drawables: 1301, lost: 0, presents: 1300)
                == "the layer gave 1301 drawables for 1300 presents of the guest")
        #expect(BenchSessionRecord.drawableNote(drawables: 1299, lost: 0, presents: 1300)
                == "the layer gave 1299 drawables for 1300 presents of the guest")
    }

    @Test func aSessionRecordsUpTo60SecondsAt120Hz() {
        #expect(BenchPlan.sessionRecordingCapacity == 7200)
    }
}
