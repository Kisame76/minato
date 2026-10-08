/// What the game view's layer recorded during a run (ADR-0037, decision 2, graphics/host's
/// MinatoMetalLayerRecording): its values for run.csv and its drawables for frames.csv. In the Simulator the layer
/// records nothing, and the app fills in the layer's values without drawables.
public struct BenchRecording: Equatable, Sendable {
    /// The QoS class of the thread that took the first drawable, as run.csv writes it; empty when unknown.
    public var qos: String
    /// MTLPixelFormat's raw value.
    public var pixelFormat: UInt
    public var drawableWidth: Int
    public var drawableHeight: Int
    public var maxDrawableCount: Int
    /// Drawable k is frame k.
    public var drawables: [BenchDrawable]
    /// Drawables past the recording's capacity, not recorded.
    public var lost: Int

    public init(qos: String, pixelFormat: UInt, drawableWidth: Int, drawableHeight: Int, maxDrawableCount: Int,
                drawables: [BenchDrawable], lost: Int) {
        self.qos = qos
        self.pixelFormat = pixelFormat
        self.drawableWidth = drawableWidth
        self.drawableHeight = drawableHeight
        self.maxDrawableCount = maxDrawableCount
        self.drawables = drawables
        self.lost = lost
    }

    /// MTLPixelFormat's case name for the formats a CAMetalLayer takes, else the raw value.
    public static func pixelFormatName(_ raw: UInt) -> String {
        switch raw {
        case 0: "invalid"
        case 70: "rgba8Unorm"
        case 71: "rgba8Unorm_srgb"
        case 80: "bgra8Unorm"
        case 81: "bgra8Unorm_srgb"
        case 90: "rgb10a2Unorm"
        case 94: "bgr10a2Unorm"
        case 115: "rgba16Float"
        default: "\(raw)"
        }
    }
}

extension BenchRunRecord {
    /// The layer's values, the drawables (lost ones too, so unequal counts show) and the first present.
    public mutating func take(_ recording: BenchRecording) {
        qos = recording.qos
        pixelFormat = BenchRecording.pixelFormatName(recording.pixelFormat)
        drawableWidth = recording.drawableWidth
        drawableHeight = recording.drawableHeight
        maxDrawableCount = recording.maxDrawableCount
        drawables = recording.drawables.count + recording.lost
        firstPresentNs = recording.drawables.first { $0.presentedNs > 0 }?.presentedNs ?? 0
    }
}

extension BenchPlan {
    /// The recording's room for a run: the draw time at the run's rate, plus 10 % for the time from the variant's
    /// start to its first frame.
    public static func recordingCapacity(seconds: Int, rate: Int) -> Int {
        (seconds * rate * 11 + 9) / 10
    }
}

extension BenchAPI {
    /// The APK in the benchmark build's Benchmark/ folder (tools/embed-benchmark.sh).
    public var apkFile: String { "bench-\(rawValue).apk" }
    /// The package it installs as (graphics/bench/android).
    public var package: String { "io.github.kisame76.minato.bench.\(rawValue)" }
    /// The API whose scene it draws, whose meshes MinatoBench counts: bench-gles31 draws bench-gles's (ADR-0044 d2)).
    public var sceneAPI: BenchAPI {
        switch self {
        case .gles, .gles31: .gles
        case .vulkan: .vulkan
        case .metal: .metal
        }
    }
}

extension BenchConfiguration {
    /// The APK of the configuration's scene in Benchmark/: ADR-0037's, or test 2's from tools/build-godot.sh.
    public var apkFile: String { scene == .godot ? "godot-bench-\(api.rawValue).apk" : api.apkFile }
    /// The package it installs as (fixtures/apps/godot-bench/export_presets.cfg for test 2's).
    public var package: String {
        scene == .godot ? "io.github.kisame76.minato.godotbench.\(api.rawValue)" : api.package
    }
}
