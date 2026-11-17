import Testing

@testable import APKBench

/// ADR-0037, step d: what the layer's recording (step b) gives run.csv, and the APKs the benchmark build embeds.
@Suite("BenchRecording")
struct BenchRecordingTests {
    /// The recording's room: the draw time at the run's rate, and 10 % for the time to the first frame.
    @Test func capacityCoversTheDrawTime() {
        #expect(BenchPlan.recordingCapacity(seconds: 52, rate: 120) == 6864)
        #expect(BenchPlan.recordingCapacity(seconds: 52, rate: 60) == 3432)
        #expect(BenchPlan.recordingCapacity(seconds: 5, rate: 120) == 660)
        #expect(BenchPlan.recordingCapacity(seconds: 1, rate: 61) == 68)
    }

    @Test func pixelFormatsByName() {
        #expect(BenchRecording.pixelFormatName(80) == "bgra8Unorm")
        #expect(BenchRecording.pixelFormatName(81) == "bgra8Unorm_srgb")
        #expect(BenchRecording.pixelFormatName(70) == "rgba8Unorm")
        #expect(BenchRecording.pixelFormatName(115) == "rgba16Float")
        #expect(BenchRecording.pixelFormatName(94) == "bgr10a2Unorm")
        #expect(BenchRecording.pixelFormatName(0) == "invalid")
        #expect(BenchRecording.pixelFormatName(555) == "555")
    }

    /// The layer's values go to run.csv; the first present is the first drawable that reached the screen.
    @Test func recordingFillsTheRecord() {
        var record = BenchRunRecord(run: BenchRun("gles-120-light-apk-01")!)
        record.take(BenchRecording(qos: "default", pixelFormat: 80, drawableWidth: 1290, drawableHeight: 2796,
                                   maxDrawableCount: 3,
                                   drawables: [BenchDrawable(nextDrawableNs: 100, presentedNs: 0),
                                               BenchDrawable(nextDrawableNs: 200, presentedNs: 9_000),
                                               BenchDrawable(nextDrawableNs: 300, presentedNs: 17_000)],
                                   lost: 0))
        #expect(record.qos == "default")
        #expect(record.pixelFormat == "bgra8Unorm")
        #expect(record.drawableWidth == 1290 && record.drawableHeight == 2796 && record.maxDrawableCount == 3)
        #expect(record.drawables == 3)
        #expect(record.firstPresentNs == 9_000)
    }

    /// Drawables past the capacity count, so the scene's frames and the drawables differ and the run is invalid.
    @Test func lostDrawablesCount() {
        var record = BenchRunRecord(run: BenchRun("gles-120-light-native-01")!)
        record.sceneFrames = 3
        record.take(BenchRecording(qos: "user-interactive", pixelFormat: 80, drawableWidth: 10, drawableHeight: 20,
                                   maxDrawableCount: 3,
                                   drawables: [BenchDrawable(nextDrawableNs: 1, presentedNs: 2),
                                               BenchDrawable(nextDrawableNs: 3, presentedNs: 4)],
                                   lost: 1))
        #expect(record.drawables == 3)
        record.take(BenchRecording(qos: "", pixelFormat: 80, drawableWidth: 10, drawableHeight: 20, maxDrawableCount: 3,
                                   drawables: [], lost: 0))
        #expect(record.drawables == 0 && record.firstPresentNs == 0)
        #expect(RunValidity.reasons(for: record).contains("frames 3, drawables 0"))
    }

    /// A guest that draws into a SurfaceView (Godot's GodotView) presents on the surface's layer, not on the
    /// window's (ADR-0043 c4): the run takes the layer with the most drawables, lost ones too, and counts the
    /// others'.
    @Test func theLayerWithTheMostDrawablesIsTheRuns() {
        func recording(_ drawables: Int, lost: Int = 0, width: Int = 10) -> BenchRecording {
            BenchRecording(qos: "default", pixelFormat: 80, drawableWidth: width, drawableHeight: 20,
                           maxDrawableCount: 3,
                           drawables: (0..<drawables).map { BenchDrawable(nextDrawableNs: Int64($0), presentedNs: 1) },
                           lost: lost)
        }
        let window = recording(2), surface = recording(3119, width: 2796)
        let chosen = BenchRecording.presenting([window, surface])
        #expect(chosen?.recording == surface)
        #expect(chosen?.others == 2)
        // Lost drawables count: a surface past the capacity still outnumbers the window.
        #expect(BenchRecording.presenting([recording(5), recording(3, lost: 4)])?.recording == recording(3, lost: 4))
        // On a tie the first, the window's, which the layer hook gives first.
        #expect(BenchRecording.presenting([recording(4), recording(4, width: 7)])?.recording == recording(4))
        #expect(BenchRecording.presenting([window])?.others == 0)
        #expect(BenchRecording.presenting([]) == nil)
    }

    /// ADR-0042, decision 4: Godot's APK frames are the recording's last drawables, the ones before are its boot
    /// image; a recording that lost drawables lost its end and gives -1.
    @Test func godotsApkFramesAreTheRecordingsLast() {
        func recording(_ count: Int, lost: Int = 0) -> BenchRecording {
            BenchRecording(qos: "default", pixelFormat: 80, drawableWidth: 10, drawableHeight: 20, maxDrawableCount: 3,
                           drawables: (0..<count).map { BenchDrawable(nextDrawableNs: Int64($0) * 10, presentedNs: 1) },
                           lost: lost)
        }
        let aligned = recording(5).alignedAtEnd(frames: 3)
        #expect(aligned.before == 2)
        #expect(aligned.drawables.map(\.nextDrawableNs) == [20, 30, 40])
        #expect(recording(3).alignedAtEnd(frames: 3).before == 0)
        // Fewer drawables than frames: nothing before, and the counts differ (RunValidity).
        #expect(recording(2).alignedAtEnd(frames: 3).before == 0)
        #expect(recording(2).alignedAtEnd(frames: 3).drawables.count == 2)
        let lost = recording(5, lost: 1).alignedAtEnd(frames: 3)
        #expect(lost.before == -1 && lost.drawables.count == 5)
        // The drawables taken after the done file was written: the done frame's own, and any later one.
        #expect(recording(5).drawablesTaken(after: 25) == 2)
        #expect(recording(5).drawablesTaken(after: 40) == 0)
    }

    /// A configuration's APK in the bundle's Benchmark/ folder and the package it installs as. ADR-0044 d2):
    /// bench-gles31 is bench-gles's scene under an ES 3.1 manifest, with its own APK and package; its runs draw
    /// bench-gles's meshes.
    @Test func aConfigurationsAPKAndTheES31Twin() {
        #expect(BenchConfiguration("vulkan-120-light")!.apkFile == "bench-vulkan.apk")
        #expect(BenchConfiguration("godot-vulkan-120-light")!.apkFile == "godot-bench-vulkan.apk")
        #expect(BenchConfiguration("godot-gles-60-light")!.package == "io.github.kisame76.minato.godotbench.gles")
        #expect(BenchConfiguration("gles-60-light")!.package == "io.github.kisame76.minato.bench.gles")
        #expect(BenchAPI.gles31.apkFile == "bench-gles31.apk")
        #expect(BenchAPI.gles31.package == "io.github.kisame76.minato.bench.gles31")
        #expect(BenchConfiguration("gles31-120-heavy")?.apkFile == "bench-gles31.apk")
        #expect(BenchAPI.gles31.sceneAPI == .gles)
        for api in BenchAPI.allCases where api != .gles31 {
            #expect(api.sceneAPI == api, "\(api)")
        }
    }
}
