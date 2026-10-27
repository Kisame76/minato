import Testing

@testable import APKBench

/// ADR-0037, the interface with the scene of step a: bench.conf in, scene.csv out.
@Suite("BenchScene")
struct BenchSceneTests {
    @Test func benchConfNamesTheRun() {
        #expect(BenchConf.text(for: BenchRun("vulkan-60-light-apk-04")!) == """
            run=vulkan-60-light-apk-04
            rate=60
            load=light
            seconds=52

            """)
        #expect(BenchConf.text(for: BenchRun("gles-120-heavy-native-00")!, seconds: 3).hasSuffix("seconds=3\n"))
    }

    /// M6's MetalFX plan, step 0: the scene's render scale, in percent, is a line of bench.conf only when it is
    /// not 100.
    @Test func benchConfCarriesTheRenderScaleOnlyWhenItIsNot100() {
        let run = BenchRun("gles-120-heavy-native-01")!
        #expect(BenchConf.text(for: run, scale: 100) == BenchConf.text(for: run))
        #expect(BenchConf.text(for: run, seconds: 3, scale: 67) == """
            run=gles-120-heavy-native-01
            rate=120
            load=heavy
            seconds=3
            scale=67

            """)
    }

    /// Test 2's done file (ADR-0042 c): "<frames> frames", written after scene.csv.
    @Test func theDoneFileCountsTheFrames() {
        #expect(BenchDone.frames("6240 frames\n") == 6240)
        #expect(BenchDone.frames("0 frames\n") == 0)
        for bad in ["", "6240 frames", "frames\n", "-1 frames\n", "6240 frame\n", "x frames\n"] {
            #expect(BenchDone.frames(bad) == nil, "\(bad)")
        }
    }

    @Test func sceneCSVReads() throws {
        let frames = try BenchCSV.scene("index,tick_ns,cpu_ns\n0,100,2000000\n1,8333433,1900000\n")
        #expect(frames == [BenchSceneFrame(index: 0, tickNs: 100, cpuNs: 2_000_000),
                           BenchSceneFrame(index: 1, tickNs: 8_333_433, cpuNs: 1_900_000)])
        #expect(try BenchCSV.scene("index,tick_ns,cpu_ns\n") == [])
    }

    @Test func aBrokenSceneCSVIsAnError() {
        for bad in ["", "index,tick,cpu\n0,1,2\n", "index,tick_ns,cpu_ns\n0,1\n", "index,tick_ns,cpu_ns\n0,x,2\n",
                    "index,tick_ns,cpu_ns\n-1,1,2\n"] {
            #expect(throws: BenchCSVError.self, "\(bad)") { try BenchCSV.scene(bad) }
        }
    }

    /// Frame k with drawable k; unequal counts make the run invalid (RunValidity), not the join.
    @Test func framesJoinByIndex() {
        let drawables = [BenchDrawable(nextDrawableNs: 110, presentedNs: 8_433_433),
                         BenchDrawable(nextDrawableNs: 8_333_500, presentedNs: 0),
                         BenchDrawable(nextDrawableNs: 16_666_900, presentedNs: 25_100_000)]
        let scene = [BenchSceneFrame(index: 1, tickNs: 8_333_433, cpuNs: 1_900_000),
                     BenchSceneFrame(index: 0, tickNs: 100, cpuNs: 2_000_000),
                     BenchSceneFrame(index: 7, tickNs: 1, cpuNs: 1)]
        #expect(BenchCSV.join(drawables: drawables, scene: scene) == [
            BenchFrame(index: 0, tickNs: 100, nextDrawableNs: 110, presentedNs: 8_433_433, cpuNs: 2_000_000),
            BenchFrame(index: 1, tickNs: 8_333_433, nextDrawableNs: 8_333_500, presentedNs: 0, cpuNs: 1_900_000),
        ])
    }
}
