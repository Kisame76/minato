import APKFormats
import Foundation
import Testing

/// The system features of bundletool's device spec (ADR-0049 decision 2's follow-up) are those Minato's
/// PackageManager reports, whose table is framework's SystemFeatures.java: a change to one must reach the other.
@Suite("System features")
struct SystemFeaturesTests {
    static let java = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .appending(path: "framework/java/com/android/minato/pm/SystemFeatures.java")

    @Test func theyAreTheFrameworksTable() throws {
        let text = try String(contentsOf: Self.java, encoding: .utf8)
        let names = Set(text.matches(of: #/features\.put\("([^"]+)", /#).map { String($0.1) })
        let vulkan = try #require(text.firstMatch(of: #/VULKAN = Set\.of\(([^)]*)\)/#))
        #expect(SystemFeatures.names == names)
        #expect(SystemFeatures.vulkan == Set(vulkan.1.matches(of: #/"([^"]+)"/#).map { String($0.1) }))
    }
}
