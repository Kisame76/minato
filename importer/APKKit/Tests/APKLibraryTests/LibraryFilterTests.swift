import APKKit
import Foundation
import Testing

@testable import APKLibrary

/// The library's filter, from its "…" menu as Photos' (All Apps, Favorites, Ready, Not Ready): which apps each choice
/// shows, and that dragging is off under one.
@Suite("Library filter")
struct LibraryFilterTests {
    private static func entry(_ id: String, _ state: LibraryEntry.State) -> LibraryEntry {
        LibraryEntry(id: id, displayName: id, state: state, report: nil,
                     base: PackageFile(fileName: "base.apk", size: 1, sha256: ""), splits: [], obbs: [],
                     source: SourceFile(fileName: "\(id).apk", size: 1, modifiedMilliseconds: 0), iconFile: nil,
                     importedAt: Date(timeIntervalSince1970: 0))
    }

    /// In the manual order: a ready, b needs splits, c incompatible, d ready, e a failed import.
    private let entries = [
        entry("a", .ready), entry("b", .needsSplits), entry("c", .incompatible), entry("d", .ready),
        entry("e", .invalid("not a zip")),
    ]

    private func ids(_ filter: LibraryFilter, favorites: [String] = []) -> [String] {
        filter.filtered(entries, favorites: favorites).map(\.id)
    }

    /// All Apps: every entry in its order; Ready: the ready apps; Not Ready: what cannot start yet (needs split APKs,
    /// incompatible, a failed import); Favorites: the favorites in the library's order, deleted apps skipped.
    @Test(arguments: [
        (LibraryFilter.all, [], ["a", "b", "c", "d", "e"]),
        (.ready, [], ["a", "d"]),
        (.notReady, [], ["b", "c", "e"]),
        (.favorites, ["d", "x", "b"], ["b", "d"]),
        (.favorites, [], []),
    ] as [(LibraryFilter, [String], [String])])
    func aFilterShowsItsApps(filter: LibraryFilter, favorites: [String], shown: [String]) {
        #expect(ids(filter, favorites: favorites) == shown)
    }

    @Test func aFilterWithoutMatchesIsEmpty() {
        #expect(LibraryFilter.ready.filtered([Self.entry("b", .needsSplits)], favorites: []).isEmpty)
    }

    @Test func picksStillBeingInstalledShowOnlyUnderAllApps() {
        #expect(LibraryFilter.allCases.filter(\.showsPendingImports) == [.all])
    }

    @Test func appsAreDraggedOnlyInTheManualOrderWithoutAFilterOrASearch() {
        #expect(LibraryArrangement().movesApps(searching: false, filter: .all))
        for filter in [LibraryFilter.favorites, .ready, .notReady] {
            #expect(!LibraryArrangement().movesApps(searching: false, filter: filter))
        }
        #expect(!LibraryArrangement().movesApps(searching: true, filter: .all))
        #expect(!LibraryArrangement(sort: .name, ascending: true).movesApps(searching: false, filter: .all))
    }

    @Test func theChoiceIsStoredByName() {
        #expect(LibraryFilter.allCases.map(\.rawValue) == ["all", "favorites", "ready", "notReady"])
        #expect(LibraryFilter(rawValue: "ready") == .ready)
    }
}
