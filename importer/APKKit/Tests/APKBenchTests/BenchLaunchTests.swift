import Foundation
import Testing

@testable import APKBench

/// ADR-0037, decision 3, and ADR-0042 c): -runBenchmark <run> makes a process a benchmark run, which shows nothing
/// else; only a twin run of test 2 may open MinatoGodot.framework.
@Suite("BenchLaunch")
struct BenchLaunchTests {
    @Test func aRunComesFromItsArgument() {
        #expect(BenchLaunch.run(in: ["Minato", "-runBenchmark", "gles-120-heavy-apk-03"])?.description
            == "gles-120-heavy-apk-03")
        let seconds = ["Minato", "-runBenchmark", "godot-metal-60-light-twin-01", "-benchmarkSeconds", "5"]
        #expect(BenchLaunch.run(in: seconds)?.description == "godot-metal-60-light-twin-01")
        for arguments in [["Minato"], ["Minato", "-runBenchmark"],
                          ["Minato", "-runBenchmark", "godot-metal-60-light-apk-01"],
                          ["Minato", "runBenchmark", "gles-120-heavy-apk-03"]] {
            #expect(BenchLaunch.run(in: arguments) == nil, "\(arguments)")
        }
    }

    /// The twin is opened only in a process whose run is a twin run, and such a process shows the run alone.
    @Test func onlyATwinRunOpensTheTwin() {
        #expect(BenchLaunch.mayOpenTwin(["Minato", "-runBenchmark", "godot-vulkan-120-light-twin-01"]))
        #expect(BenchLaunch.mayOpenTwin(["Minato", "-runBenchmark", "godot-gles-60-light-twin-02"]))
        for arguments in [["Minato"], ["Minato", "-runBenchmark", "godot-vulkan-120-light-apk-01"],
                          ["Minato", "-runBenchmark", "vulkan-120-light-native-01"],
                          ["Minato", "-recordSession", "io.github.kisame76.minato.bench.gles"]] {
            #expect(!BenchLaunch.mayOpenTwin(arguments), "\(arguments)")
        }
    }

    /// bench.gd's meshes per load, and the driver and renderer the twin starts Godot with.
    @Test func theTwinsGodotSettings() {
        #expect(BenchConfiguration("godot-gles-60-light")!.godotMeshes == 512)
        #expect(BenchConfiguration("godot-vulkan-120-heavy")!.godotMeshes == 2048)
        #expect(BenchConfiguration("gles-60-light")!.godotMeshes == nil)
        #expect(BenchConfiguration("godot-gles-60-light")!.godotArguments
            == ["--rendering-driver", "opengl3", "--rendering-method", "gl_compatibility", "--",
                "--bench-driver=opengl3"])
        #expect(BenchConfiguration("godot-vulkan-60-light")!.godotArguments
            == ["--rendering-driver", "vulkan", "--rendering-method", "mobile", "--", "--bench-driver=vulkan"])
        #expect(BenchConfiguration("godot-metal-60-light")!.godotArguments
            == ["--rendering-driver", "metal", "--rendering-method", "mobile", "--", "--bench-driver=metal"])
    }

    /// godotMeshes repeats bench.gd's LOADS, which ADR-0042 d) calibrates: a change to one must reach the other.
    @Test func theMeshesAreBenchGDsLoads() throws {
        let script = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appending(path: "fixtures/apps/godot-bench/bench.gd")
        let text = try String(contentsOf: script, encoding: .utf8)
        let loads = try #require(text.firstMatch(of: #/const LOADS := \{"light": *(\d+), *"heavy": *(\d+)\}/#))
        #expect(BenchConfiguration("godot-gles-60-light")!.godotMeshes == Int(loads.1))
        #expect(BenchConfiguration("godot-vulkan-120-heavy")!.godotMeshes == Int(loads.2))
    }
}
