import APKFormats
import Foundation
import Testing

/// A set's folder name and code path (ADR-0048, decision 6).
@Suite("Set digest")
struct SetDigestTests {
    static let base = (splitName: String?.none, sha256: String(repeating: "a", count: 64))
    static let split = (splitName: String?.some("config.de"), sha256: String(repeating: "b", count: 64))

    @Test func installedNames() {
        #expect(SetDigest.installedName(of: nil) == "base.apk")
        #expect(SetDigest.installedName(of: "config.arm64_v8a") == "split_config.arm64_v8a.apk")
    }

    @Test func theSha256OverTheSortedLines() throws {
        // printf 'base.apk\0aaa…a\nsplit_config.de.apk\0bbb…b\n' | shasum -a 256
        #expect(try SetDigest.of([Self.split, Self.base])
            == "1e9d1e8d9aa41718dc4f508b7536c96c13a7b53b6eca54a3b2a4fb6044b4b1b0")
    }

    @Test func theOrderOfTheFilesDoesNotMatter() throws {
        #expect(try SetDigest.of([Self.base, Self.split]) == SetDigest.of([Self.split, Self.base]))
        let one = (splitName: String?.none, sha256: "1"), two = (splitName: String?.none, sha256: "2")
        #expect(try SetDigest.of([one, two]) == SetDigest.of([two, one]))
    }

    @Test func aSeparatorInsideAFieldIsRefused() {
        // ("a\0b", "c") and ("a", "b\0c") would read alike.
        #expect(throws: SetDigestError.separatorInField) { try SetDigest.of([(splitName: "a\0b", sha256: "c")]) }
        #expect(throws: SetDigestError.separatorInField) { try SetDigest.of([(splitName: nil, sha256: "b\nc")]) }
    }

    @Test func everyByteCounts() throws {
        let other = (splitName: String?.some("config.de"), sha256: String(repeating: "c", count: 64))
        #expect(try SetDigest.of([Self.base, Self.split]) != SetDigest.of([Self.base, other]))
        #expect(try SetDigest.of([Self.base, Self.split]) != SetDigest.of([Self.base]))
    }
}
