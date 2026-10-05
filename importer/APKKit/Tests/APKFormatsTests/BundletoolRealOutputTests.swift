import APKFormats
import Foundation
import Testing

/// bundletool's own `.apks`, which `tools/formats-check.sh` builds and passes in `FORMATS_CHECK_DIR` with what
/// `extract-apks` chose from each for the same device spec (ADR-0049 test 2). Skipped in a plain `swift test`.
@Suite("bundletool's real output", .enabled(if: ProcessInfo.processInfo.environment["FORMATS_CHECK_DIR"] != nil))
struct BundletoolRealOutputTests {
    /// One `.apks` and the base names `extract-apks` wrote: the install with the on-demand modules asked for, and
    /// each pack's own on top of it.
    struct Case: Decodable {
        var apks: String
        var install: [String]
        var packs: [String: [String]]
    }

    static let folder = URL(fileURLWithPath: ProcessInfo.processInfo.environment["FORMATS_CHECK_DIR"] ?? "/nonexistent")

    /// The spec formats-check.sh gives extract-apks: arm64-v8a, 480 dpi, SDK 35 without the SDK runtime, ASTC and
    /// ETC1 by their GL extensions and ETC2 by OpenGL ES 3.0.
    static var device: BundletoolDevice {
        var device = BundletoolDevice(DeviceProfile.iPhone(nativeScale: 3, languages: ["en-US"]))
        device.features = ["reqGlEsVersion=0x30000"]
        return device
    }

    static func cases() throws -> [Case] {
        try JSONDecoder().decode([Case].self, from: Data(contentsOf: folder.appendingPathComponent("cases.json")))
    }

    @Test func eachApksGivesWhatExtractApksTakes() throws {
        let cases = try Self.cases()
        #expect(cases.count >= 3)
        for testCase in cases {
            let container = try Container.open(Self.folder.appendingPathComponent(testCase.apks))
            #expect(container.kind == .bundletool, "\(testCase.apks)")
            let (items, choice) = try container.bundletoolItems(for: Self.device)
            #expect(items.map { ($0.entry.name as NSString).lastPathComponent }.sorted() == testCase.install.sorted(),
                    "\(testCase.apks)")
            #expect(Dictionary(uniqueKeysWithValues: choice.packs.map { ($0.name, $0.paths.map { ($0 as NSString).lastPathComponent }.sorted()) })
                == testCase.packs.mapValues { $0.sorted() }, "\(testCase.apks)")
        }
    }

    @Test func theTestsTocWriterWritesWhatProtocDecodes() throws {
        // formats-check.sh decodes this file with protoc against bundletool's commands.proto.
        try TocWriter.toc(BundletoolTocTests.sample).write(to: Self.folder.appendingPathComponent("sample-toc.pb"))
    }
}
