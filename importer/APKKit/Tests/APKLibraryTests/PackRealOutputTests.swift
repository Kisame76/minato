import APKFormats
import Foundation
import Testing

@testable import APKLibrary

/// A set bundletool itself built, with a fast-follow and an on-demand pack: tools/build-packs-test.sh's
/// build/packs-test/packs-test.apks, which tools/formats-check.sh passes in `PACKS_TEST_APKS` (ADR-0049 f's fix: the
/// synthetic slices of PackImportTests carried a versionCode, which bundletool's later slices have not). Skipped in a
/// plain `swift test`.
@Suite("A real set's packs", .enabled(if: ProcessInfo.processInfo.environment["PACKS_TEST_APKS"] != nil))
struct PackRealOutputTests {
    @Test func bothLaterPacksOfARealSetLandInPacks() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let apks = try Data(contentsOf: URL(fileURLWithPath: ProcessInfo.processInfo.environment["PACKS_TEST_APKS"] ?? ""))

        let results = try await Library(paths: sandbox.paths)
            .importUnit(SetImportTests.unit(["packs-test.apks": apks], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1 && entry.state == .ready)
        #expect(entry.id == "io.github.kisame76.minato.test.packs")
        #expect(entry.packs?.map(\.fileName).sorted() == ["fastfollow-master.apk", "ondemand-master.apk"])
        #expect(entry.ignoredFiles == nil)
        #expect(try PackImportTests.packFiles(entry, sandbox.paths) == ["fastfollow-master.apk", "ondemand-master.apk"])
    }
}
