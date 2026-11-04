import Foundation

/// The library's sort orders, as the Files app's "Sort By" (navigation design,
/// section 3). Equal ones keep the manual order.
public enum LibrarySort: String, Sendable, CaseIterable, Codable {
    /// The order of the index's entries: the import order, until the user moves an app. It has no direction.
    case manual
    case name
    /// The first import: an update keeps it.
    case dateAdded
    /// Apps that were never opened go to the end, in either direction.
    case lastOpened
    /// The installed files: the APKs and OBBs, without the app's data.
    case size

    public func sorted(_ entries: [LibraryEntry], ascending: Bool, lastOpened: [String: Date]) -> [LibraryEntry] {
        let ordered = { (before: (LibraryEntry, LibraryEntry) -> Bool?) -> [LibraryEntry] in
            entries.enumerated().sorted { left, right in
                before(left.element, right.element) ?? (left.offset < right.offset)
            }.map(\.element)
        }
        /// Whether `left` goes before `right` by `order`, nil when they are equal.
        func by<Value: Comparable>(_ left: Value, _ right: Value) -> Bool? {
            left == right ? nil : (ascending ? left < right : left > right)
        }
        switch self {
        case .manual:
            return entries
        case .name:
            return ordered { left, right in
                switch left.displayName.localizedStandardCompare(right.displayName) {
                case .orderedSame: nil
                case .orderedAscending: ascending
                case .orderedDescending: !ascending
                }
            }
        case .dateAdded:
            return ordered { by($0.importedAt, $1.importedAt) }
        case .lastOpened:
            return ordered { left, right in
                switch (lastOpened[left.id], lastOpened[right.id]) {
                case let (left?, right?): by(left, right)
                case (.some, nil): true
                case (nil, .some): false
                case (nil, nil): nil
                }
            }
        case .size:
            return ordered { by($0.installedSize, $1.installedSize) }
        }
    }

    /// Home's Recently Opened: the `count` apps opened last, the newest first; an app no longer installed is none.
    public static func recentlyOpened(_ entries: [LibraryEntry], lastOpened: [String: Date],
                                      count: Int = 4) -> [LibraryEntry] {
        Array(LibrarySort.lastOpened.sorted(entries.filter { lastOpened[$0.id] != nil }, ascending: false,
                                            lastOpened: lastOpened).prefix(count))
    }

    /// Home's Favorites: the favorites' entries in the favorites' own order; an app no longer installed is none.
    public static func favorites(_ entries: [LibraryEntry], favorites: [String]) -> [LibraryEntry] {
        let byID = Dictionary(entries.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        return favorites.compactMap { byID[$0] }
    }
}

/// The library's order as its "…" menu sets it (the navigation design, section 3): the sort order and its direction, a
/// preference of the device.
public struct LibraryArrangement: Equatable, Sendable {
    public var sort: LibrarySort
    /// Manual has none.
    public var ascending: Bool

    public init(sort: LibrarySort = .manual, ascending: Bool = true) {
        self.sort = sort
        self.ascending = ascending
    }

    /// A tap on `sort` in Sort By, as in Files: the active order flips its direction, another one starts with its own,
    /// A to Z by name and the newest or the largest first otherwise.
    public func choosing(_ sort: LibrarySort) -> LibraryArrangement {
        switch sort {
        case .manual: LibraryArrangement()
        case self.sort: LibraryArrangement(sort: sort, ascending: !ascending)
        case .name: LibraryArrangement(sort: sort, ascending: true)
        case .dateAdded, .lastOpened, .size: LibraryArrangement(sort: sort, ascending: false)
        }
    }

    /// Apps are dragged only in the manual order, so that a sort order never overwrites it, and not in a search's
    /// results or under a filter, which have gaps.
    public func movesApps(searching: Bool, filter: LibraryFilter = .all) -> Bool {
        sort == .manual && !searching && filter == .all
    }

    public func arranged(_ entries: [LibraryEntry], lastOpened: [String: Date]) -> [LibraryEntry] {
        sort.sorted(entries, ascending: ascending, lastOpened: lastOpened)
    }
}

/// The library's filter, from its "…" menu as Photos' (the navigation design, section 3): a preference of the device
/// that holds across launches, and the search works inside it.
public enum LibraryFilter: String, Sendable, CaseIterable, Codable {
    case all
    case favorites
    case ready
    /// Needs split APKs, incompatible, or a failed import.
    case notReady

    /// The entries this filter shows, in the order they came in; the favorites' own order does not count.
    public func filtered(_ entries: [LibraryEntry], favorites: [String]) -> [LibraryEntry] {
        switch self {
        case .all:
            return entries
        case .favorites:
            let wanted = Set(favorites)
            return entries.filter { wanted.contains($0.id) }
        case .ready:
            return entries.filter { $0.state == .ready }
        case .notReady:
            return entries.filter { $0.state != .ready }
        }
    }

    /// Picks that are still being installed are no app yet: only the whole library lists them.
    public var showsPendingImports: Bool { self == .all }
}

extension LibraryEntry {
    /// The installed files' bytes: the base APK, the splits and the OBBs.
    public var installedSize: UInt64 {
        ([base].compactMap { $0 } + splits + obbs).reduce(0) { $0 + $1.size }
    }
}
