import APKFormats
import APKKit
import Foundation
import Testing

@testable import APKLibrary

/// Imports each file of tools/make-format-files.py through the entry the app's "+" and Open with use (`Library.adopt`,
/// then `importUnit`), into a temporary library, and checks what a user sees: the status, the Split APKs, the OBBs in
/// the app's data folder and the asset packs. The files come from the folder that MINATO_FORMAT_FILES names (made from
/// our own fixtures); the suite is off without it. The libraries are imported without the converter, as the other
/// import tests do.
enum FormatFiles {
    static var folder: URL? {
        ProcessInfo.processInfo.environment["MINATO_FORMAT_FILES"].map { URL(fileURLWithPath: $0, isDirectory: true) }
    }
    static func file(_ name: String) -> URL { folder!.appendingPathComponent(name) }
    static let splits = "io.github.kisame76.minato.test.splits"
    static let obb = "io.github.kisame76.minato.test.obb"
    static let packs = "io.github.kisame76.minato.test.packs"
    static let javaTest = "io.github.kisame76.minato.test.javatest"
    static let setFiles = ["base.apk", "split_config.arm64_v8a.apk", "split_config.en.apk", "split_feature.apk"]
    static let splitNames = ["config.arm64_v8a", "config.en", "feature"]
}

@Suite("Format files", .enabled(if: FormatFiles.folder != nil))
struct FormatFilesTests {
    typealias Sandbox = LibraryTests.Sandbox

    /// "+" with these picks (files, or a folder), then the unit's import.
    static func pick(_ picks: [URL], into library: Library) async throws -> [ImportResult] {
        try await library.importUnit(library.adopt(picks))
    }

    static func describe(_ results: [ImportResult]) -> String {
        results.map { result in
            let names = result.entry.splits.compactMap(\.splitName).sorted()
            return "\(result.entry.id) \(result.entry.state) \(result.decision) splits=\(names)"
        }.joined(separator: "; ")
    }

    static func splitNames(_ results: [ImportResult]) -> [String] {
        results.first?.entry.splits.compactMap(\.splitName).sorted() ?? []
    }

    static func obbNames(_ paths: LibraryPaths, _ package: String) -> [String] {
        let folder = paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true)
        return ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    @Test func aPlainAPKIsReady() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let results = try await Self.pick([FormatFiles.file("01-plain.apk")], into: library)
        #expect(results.count == 1, "\(Self.describe(results))")
        #expect(results.first?.entry.id == FormatFiles.javaTest, "\(Self.describe(results))")
        #expect(results.first?.entry.state == .ready, "\(Self.describe(results))")
    }

    @Test func fourFilesPickedTogetherGiveFourReadyApps() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let names = ["01-plain.apk", "04-with-obb.xapk", "06-asset-packs.apks", "02-bundletool.apks"]
        let results = try await Self.pick(names.map(FormatFiles.file), into: library)
        let byID = Dictionary(uniqueKeysWithValues: results.map { ($0.entry.id, $0.entry) })
        let expected: Set = [FormatFiles.javaTest, FormatFiles.obb, FormatFiles.packs, FormatFiles.splits]
        #expect(Set(byID.keys) == expected, "\(Self.describe(results))")
        for entry in byID.values { #expect(entry.state == .ready, "\(entry.id) \(entry.state)") }
        #expect(Self.splitNames(results.filter { $0.entry.id == FormatFiles.splits }) == FormatFiles.splitNames)
        #expect(Self.obbNames(sandbox.paths, FormatFiles.obb) == [
            "main.1.\(FormatFiles.obb).obb", "patch.1.\(FormatFiles.obb).obb"])
        #expect(byID[FormatFiles.packs]?.packs?.count == 2, "packs \(byID[FormatFiles.packs]?.packs ?? [])")
    }

    @Test(arguments: ["03-zip-of-apks.apks", "05-splits.apkm"])
    func aContainerOfTheSetIsReadyWithItsSplits(name: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let results = try await Self.pick([FormatFiles.file(name)], into: library)
        #expect(results.count == 1 && results.first?.entry.state == .ready, "\(name): \(Self.describe(results))")
        #expect(Self.splitNames(results) == FormatFiles.splitNames, "\(name): \(Self.describe(results))")
    }

    @Test func fourLooseAPKsPickedTogetherAreOneReadyApp() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let picks = FormatFiles.setFiles.map { FormatFiles.file("07-loose-set/\($0)") }
        let results = try await Self.pick(picks, into: library)
        #expect(results.count == 1 && results.first?.entry.state == .ready, "\(Self.describe(results))")
        #expect(Self.splitNames(results) == FormatFiles.splitNames, "\(Self.describe(results))")
    }

    @Test func theFolderOfTheSetIsOneReadyApp() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let results = try await Self.pick([FormatFiles.file("07-loose-set")], into: library)
        #expect(results.count == 1 && results.first?.entry.state == .ready, "\(Self.describe(results))")
        #expect(Self.splitNames(results) == FormatFiles.splitNames, "\(Self.describe(results))")
    }

    @Test func theBaseAloneNeedsSplitsAndPickingTheOtherThreeMakesItReady() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await Self.pick([FormatFiles.file("07-loose-set/base.apk")], into: library)
        #expect(first.first?.entry.state == .needsSplits, "base alone: \(Self.describe(first))")
        let others = FormatFiles.setFiles.dropFirst().map { FormatFiles.file("07-loose-set/\($0)") }
        let second = try await Self.pick(others, into: library)
        let entries = await library.entries
        let state = entries.first { $0.id == FormatFiles.splits }?.state
        #expect(state == .ready, "after the other three: \(String(describing: state)); \(Self.describe(second))")
    }

    @Test func anOBBPickedAfterItsAppGoesToTheAppByItsName() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let app = try await Self.pick([FormatFiles.file("08-obb-later/obb-test.apk")], into: library)
        #expect(app.first?.entry.state == .ready, "\(Self.describe(app))")
        #expect(Self.obbNames(sandbox.paths, FormatFiles.obb).isEmpty)
        let names = ["main.1.\(FormatFiles.obb).obb", "patch.1.\(FormatFiles.obb).obb"]
        let results = try await Self.pick(names.map { FormatFiles.file("08-obb-later/\($0)") }, into: library)
        let found = Self.obbNames(sandbox.paths, FormatFiles.obb)
        #expect(found == names, "after the OBBs: \(found); \(Self.describe(results))")
    }
}
