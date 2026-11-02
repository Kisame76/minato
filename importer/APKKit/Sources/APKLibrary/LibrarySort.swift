import Foundation

/// The library's sort orders, as the Files app's "Sort By" (docs/plans/2026-09-24-app-navigation-design.md, section
/// 3). Equal ones keep the manual order.
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
}

extension LibraryEntry {
    /// The installed files' bytes: the base APK, the splits and the OBBs.
    public var installedSize: UInt64 {
        ([base].compactMap { $0 } + splits + obbs).reduce(0) { $0 + $1.size }
    }
}
