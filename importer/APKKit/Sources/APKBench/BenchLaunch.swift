/// How a process starts (ADR-0037, decision 3): with -runBenchmark <run> it is one benchmark run and shows nothing
/// else, so the library never runs in it.
public enum BenchLaunch {
    /// The run of `-runBenchmark <run>`, nil without one.
    public static func run(in arguments: [String]) -> BenchRun? {
        guard let flag = arguments.firstIndex(of: "-runBenchmark"), arguments.indices.contains(flag + 1) else {
            return nil
        }
        return BenchRun(arguments[flag + 1])
    }

    /// Test 2's twin (ADR-0042, decision 3) may be opened only in a twin run: a process that loads Godot is a
    /// benchmark run, which ends after its run.
    public static func mayOpenTwin(_ arguments: [String]) -> Bool {
        run(in: arguments)?.variant == .twin
    }
}

extension BenchConfiguration {
    /// Test 2's meshes per load, as bench.gd's LOADS; nil for ADR-0037's scene, whose MinatoBench answers.
    public var godotMeshes: Int? {
        guard scene == .godot else { return nil }
        return load == .light ? 512 : 2048
    }

    /// The twin's arguments for Godot after the executable: the driver and the renderer of the API, then bench.gd's
    /// check of the driver; the twin adds --bench-config. GLES is Godot's Compatibility renderer on opengl3, as the
    /// GLES APK's preset asks for it.
    public var godotArguments: [String] {
        let driver = api == .gles ? "opengl3" : api.rawValue
        return ["--rendering-driver", driver, "--rendering-method", api == .gles ? "gl_compatibility" : "mobile", "--",
                "--bench-driver=\(driver)"]
    }
}
