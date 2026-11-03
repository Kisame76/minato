import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The app navigation's data (docs/plans/2026-09-24-app-navigation-design.md, step N1): the index's version 2 with
/// the manual order, the favorites and when each app was last opened, and the library's sort orders.
@Suite("Navigation data")
struct NavigationDataTests {
    typealias Sandbox = LibraryTests.Sandbox

    /// A library with an app for each of `packages`, imported in that order.
    private func library(_ sandbox: Sandbox, _ packages: [String]) async throws -> Library {
        let library = try Library(paths: sandbox.paths)
        for package in packages {
            _ = try await library.importFile(sandbox.file(SampleAPK.make(package: package, label: package),
                                                          named: "\(package).apk"))
        }
        return library
    }

    private func ids(_ library: Library) async -> [String] {
        await library.entries.map(\.id)
    }

    @Test func aMoveKeepsEveryOtherEntryAndIsSaved() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app", "c.app", "d.app"])

        try await library.move("d.app", before: "b.app")
        #expect(await ids(library) == ["a.app", "d.app", "b.app", "c.app"])
        try await library.move("a.app", before: nil)
        #expect(await ids(library) == ["d.app", "b.app", "c.app", "a.app"])
        // Before itself, or an unknown one: nothing changes.
        try await library.move("b.app", before: "b.app")
        try await library.move("x.app", before: "b.app")
        #expect(await ids(library) == ["d.app", "b.app", "c.app", "a.app"])
        #expect(try await Library(paths: sandbox.paths).entries.map(\.id) == ["d.app", "b.app", "c.app", "a.app"])
    }

    @Test func aNewImportGoesToTheEnd() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        try await library.move("b.app", before: "a.app")
        _ = try await library.importFile(sandbox.file(SampleAPK.make(package: "c.app"), named: "c.apk"))
        #expect(await ids(library) == ["b.app", "a.app", "c.app"])
    }

    @Test func anUpdateKeepsPlaceFavoriteLastOpenedAndDateAdded() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app", "c.app"])
        let before = try #require(await library.entries.first { $0.id == "b.app" })
        let opened = Date(timeIntervalSince1970: 1_800_000_000)
        try await library.setFavorite(["b.app"], true)
        try await library.recordOpened("b.app", at: opened)

        let result = try await library.importFile(sandbox.file(
            SampleAPK.make(package: "b.app", versionCode: 2, label: "b.app"), named: "b2.apk"))

        #expect(result.decision == .update)
        #expect(await ids(library) == ["a.app", "b.app", "c.app"])
        #expect(await library.favorites == ["b.app"])
        #expect(await library.lastOpened == ["b.app": opened])
        #expect(result.entry.importedAt == before.importedAt)
    }

    @Test func deletingClearsFavoriteAndLastOpened() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        try await library.setFavorite(["a.app", "b.app"], true)
        try await library.recordOpened("a.app", at: Date(timeIntervalSince1970: 1_800_000_000))

        try await library.remove(id: "a.app")

        #expect(await library.favorites == ["b.app"])
        #expect(await library.lastOpened.isEmpty)
        let reloaded = try Library(paths: sandbox.paths)
        #expect(await reloaded.favorites == ["b.app"])
        #expect(await reloaded.lastOpened.isEmpty)
    }

    @Test func favoritesKeepTheirOwnOrder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app", "c.app"])
        try await library.setFavorite(["c.app"], true)
        try await library.setFavorite(["a.app", "c.app"], true)  // c is one already: it keeps its place
        #expect(await library.favorites == ["c.app", "a.app"])
        try await library.moveFavorite("a.app", before: "c.app")
        #expect(await library.favorites == ["a.app", "c.app"])
        try await library.setFavorite(["c.app"], false)
        #expect(await library.favorites == ["a.app"])
        // Only installed apps: an unknown package is none.
        try await library.setFavorite(["x.app"], true)
        #expect(await library.favorites == ["a.app"])
        #expect(try await Library(paths: sandbox.paths).favorites == ["a.app"])
    }

    @Test func aFileThatIsNoValidAPKIsNoFavorite() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let invalid = try await library.importFile(sandbox.file(Data("not a zip".utf8), named: "broken.apk")).entry
        #expect(invalid.state != .ready)
        try await library.setFavorite([invalid.id], true)
        #expect(await library.favorites.isEmpty)
    }

    @Test func onlyAValidAPKCanBeAFavorite() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app"])
        let invalid = try await library.importFile(sandbox.file(Data("not a zip".utf8), named: "broken.apk")).entry
        #expect(await library.entries.first { $0.id == "a.app" }?.canBeFavorite == true)
        #expect(!invalid.canBeFavorite)
    }

    @Test func theSelectionsButtonAddsUnlessAllAreFavoritesAlready() {
        // The design's section 3: for everything selected; if all are favorites already, it removes them instead.
        #expect(Library.favoriteAdds(["a", "b"], favorites: ["a"]))
        #expect(!Library.favoriteAdds(["a", "b"], favorites: ["c", "b", "a"]))
    }

    @Test func aVersionOneIndexLoadsWithoutLossAndIsWrittenAsVersionTwo() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        _ = try await library(sandbox, ["a.app", "b.app"])
        // As the file holds them: ISO 8601 keeps whole seconds.
        let entries = try Library.entries(in: sandbox.paths)
        // The index as Minato wrote it before the navigation: version 1, no favorites, nothing opened.
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let entriesJSON = String(decoding: try encoder.encode(entries), as: UTF8.self)
        try Data("{\"version\":1,\"entries\":\(entriesJSON)}".utf8).write(to: sandbox.paths.indexFile)

        let library = try Library(paths: sandbox.paths)
        #expect(await library.entries == entries)
        #expect(await library.favorites.isEmpty)
        #expect(await library.lastOpened.isEmpty)
        #expect(try Library.entries(in: sandbox.paths) == entries)

        try await library.setFavorite(["b.app"], true)
        let file = try Data(contentsOf: sandbox.paths.indexFile)
        let written = try JSONSerialization.jsonObject(with: file) as? [String: Any]
        #expect(written?["version"] as? Int == 2)
        #expect(written?["favorites"] as? [String] == ["b.app"])
    }

    // MARK: - Sort orders and Recently Opened

    private static func entry(_ id: String, name: String, added: TimeInterval, size: UInt64) -> LibraryEntry {
        LibraryEntry(id: id, displayName: name, state: .ready, report: nil,
                     base: PackageFile(fileName: "base.apk", size: size, sha256: ""), splits: [], obbs: [],
                     source: SourceFile(fileName: "\(id).apk", size: size, modifiedMilliseconds: 0),
                     iconFile: nil,
                     importedAt: Date(timeIntervalSince1970: added))
    }

    /// In the manual order: b, a, c, d. Names a…d differ from it; c was never opened.
    private let entries = [
        entry("b", name: "Bravo", added: 300, size: 10),
        entry("a", name: "alpha", added: 100, size: 40),
        entry("c", name: "Charlie", added: 200, size: 30),
        entry("d", name: "delta 10", added: 400, size: 20),
    ]
    private let opened: [String: Date] = [
        "a": Date(timeIntervalSince1970: 50), "b": Date(timeIntervalSince1970: 70),
        "d": Date(timeIntervalSince1970: 60),
    ]

    private func sorted(_ order: LibrarySort, ascending: Bool) -> [String] {
        order.sorted(entries, ascending: ascending, lastOpened: opened).map(\.id)
    }

    @Test func everySortOrderInBothDirections() {
        #expect(sorted(.manual, ascending: true) == ["b", "a", "c", "d"])
        #expect(sorted(.manual, ascending: false) == ["b", "a", "c", "d"])  // Manual has no direction
        #expect(sorted(.name, ascending: true) == ["a", "b", "c", "d"])  // as Finder compares, not by case
        #expect(sorted(.name, ascending: false) == ["d", "c", "b", "a"])
        #expect(sorted(.dateAdded, ascending: true) == ["a", "c", "b", "d"])
        #expect(sorted(.dateAdded, ascending: false) == ["d", "b", "c", "a"])
        #expect(sorted(.size, ascending: true) == ["b", "d", "c", "a"])
        #expect(sorted(.size, ascending: false) == ["a", "c", "d", "b"])
    }

    @Test func neverOpenedAppsComeLastInBothDirections() {
        #expect(sorted(.lastOpened, ascending: true) == ["a", "d", "b", "c"])
        #expect(sorted(.lastOpened, ascending: false) == ["b", "d", "a", "c"])
    }

    @Test func theSizeIsTheInstalledFiles() {
        var big = Self.entry("x", name: "X", added: 0, size: 5)
        big.splits = [PackageFile(fileName: "split.apk", size: 7, sha256: "")]
        big.obbs = [PackageFile(fileName: "main.obb", size: 11, sha256: "")]
        #expect(big.installedSize == 23)
    }

    @Test func recentlyOpenedAreTheFourNewestAndSkipDeletedApps() {
        var all = entries + [Self.entry("e", name: "Echo", added: 500, size: 1),
                             Self.entry("f", name: "Foxtrot", added: 600, size: 1)]
        var times = opened
        times["e"] = Date(timeIntervalSince1970: 90)
        times["f"] = Date(timeIntervalSince1970: 80)
        times["gone"] = Date(timeIntervalSince1970: 99)
        #expect(LibrarySort.recentlyOpened(all, lastOpened: times).map(\.id) == ["e", "f", "b", "d"])
        all.removeAll { $0.id == "e" }
        #expect(LibrarySort.recentlyOpened(all, lastOpened: times).map(\.id) == ["f", "b", "d", "a"])
    }

    @Test func clearingDataKeepsTheAppItsOBBsAndItsPlace() async throws {
        // installd's clearAppData (Android 15): the contents of the app's CE and DE data, Android/data/<package> and
        // Android/media/<package> go; OBB folders stay until the app is uninstalled.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        try await library.setFavorite(["a.app"], true)
        try await library.recordOpened("a.app", at: Date(timeIntervalSince1970: 10))
        let paths = sandbox.paths
        try paths.prepareData(for: "a.app")
        try paths.prepareData(for: "b.app")
        let data = paths.dataDirectory(for: "a.app")
        let gone = ["data/shared_prefs/p.xml", "data/cache/c", "data/code_cache/k", "data_de/d", "external/files/save"]
        let kept = ["obb/main.1.a.app.obb", "download/picked.txt", "profile.json"]
        for file in gone + kept {
            let url = data.appendingPathComponent(file)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try Data("x".utf8).write(to: url)
        }
        let other = paths.dataDirectory(for: "b.app").appendingPathComponent("data/save")
        try Data("x".utf8).write(to: other)
        let before = await library.entries

        try await library.clearData(id: "a.app")

        for file in gone { #expect(!FileManager.default.fileExists(atPath: data.appendingPathComponent(file).path)) }
        for file in kept { #expect(FileManager.default.fileExists(atPath: data.appendingPathComponent(file).path)) }
        // As installd makes them at an install.
        for folder in ["data/cache", "data/code_cache", "data_de/cache", "data_de/code_cache", "external"] {
            #expect(try FileManager.default.contentsOfDirectory(atPath: data.appendingPathComponent(folder).path)
                .isEmpty)
        }
        #expect(FileManager.default.fileExists(atPath: other.path))
        #expect(await library.entries == before)
        #expect(await library.favorites == ["a.app"])
        #expect(await library.lastOpened["a.app"] == Date(timeIntervalSince1970: 10))
    }

    @Test func clearingDataOrDeletingRemovesTheKeptSessionLog() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        let log = SessionLog(folder: sandbox.paths.sessionLogDirectory)
        for package in ["a.app", "b.app"] {
            let writer = try log.writer(for: package)
            writer.append(Data("I/tag: hello\n".utf8))
            writer.close()
            try log.keep(package: package)
        }
        try await library.clearData(id: "a.app")
        #expect(!FileManager.default.fileExists(atPath: log.keptFile(for: "a.app").path))
        try await library.remove(id: "b.app")
        #expect(!FileManager.default.fileExists(atPath: log.keptFile(for: "b.app").path))
    }

    @Test func homesFavoritesKeepTheirOwnOrderAndSkipDeletedApps() {
        #expect(LibrarySort.favorites(entries, favorites: ["d", "gone", "b"]).map(\.id) == ["d", "b"])
        #expect(LibrarySort.favorites(entries, favorites: []).isEmpty)
    }

    @Test func openingAnAppRecordsItForRecentlyOpened() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        try await library.recordOpened("a.app", at: Date(timeIntervalSince1970: 10))
        try await library.recordOpened("b.app", at: Date(timeIntervalSince1970: 20))
        let reopened = try Library(paths: sandbox.paths)
        #expect(LibrarySort.recentlyOpened(await reopened.entries, lastOpened: await reopened.lastOpened).map(\.id)
            == ["b.app", "a.app"])
    }
}
