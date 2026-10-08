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

    /// Each API's APK in the bundle's Benchmark/ folder, and the package it installs as.
    @Test func apksByAPI() {
        #expect(BenchAPI.gles.apkFile == "bench-gles.apk")
        #expect(BenchAPI.vulkan.apkFile == "bench-vulkan.apk")
        #expect(BenchAPI.gles.package == "io.github.kisame76.minato.bench.gles")
        #expect(BenchAPI.vulkan.package == "io.github.kisame76.minato.bench.vulkan")
        #expect(BenchConfiguration("vulkan-120-light")!.apkFile == "bench-vulkan.apk")
        #expect(BenchConfiguration("godot-vulkan-120-light")!.apkFile == "godot-bench-vulkan.apk")
        #expect(BenchConfiguration("godot-gles-60-light")!.package == "io.github.kisame76.minato.godotbench.gles")
        #expect(BenchConfiguration("gles-60-light")!.package == "io.github.kisame76.minato.bench.gles")
    }

    /// ADR-0044 d2): bench-gles31 is bench-gles's scene under an ES 3.1 manifest, with its own APK and package; its
    /// runs draw bench-gles's meshes.
    @Test func theES31TwinIsBenchGLESsScene() {
        #expect(BenchAPI.gles31.apkFile == "bench-gles31.apk")
        #expect(BenchAPI.gles31.package == "io.github.kisame76.minato.bench.gles31")
        #expect(BenchConfiguration("gles31-120-heavy")?.apkFile == "bench-gles31.apk")
        #expect(BenchAPI.gles31.sceneAPI == .gles)
        for api in BenchAPI.allCases where api != .gles31 {
            #expect(api.sceneAPI == api, "\(api)")
        }
    }
}
