import Darwin
import Foundation

/// One app's space for Settings › Storage (docs/plans/2026-09-24-app-navigation-design.md, section 6), as the iPhone
/// Storage screen shows App Size and Documents & Data: `app` is its install folder, the installed files and their
/// converted code; `data` what it keeps and what its starts made, its data folders with the OBBs, the write-back's
/// records, its guest /data in Caches and its kept session log. Bytes the disk gives them.
public struct StorageUse: Sendable, Equatable {
    public let id: String
    public let app: UInt64
    public let data: UInt64

    public var total: UInt64 { app + data }
}

extension Library {
    /// Every installed app's space, largest first, apps of the same size in the library's order. Files that could not
    /// be read as an APK take none and are left out.
    public func storage() -> [StorageUse] {
        let uses = entries.filter { !$0.id.hasPrefix("file:") && Self.isSafePackageName($0.id) }.map { entry in
            let id = entry.id
            var counted = Set<FileIdentity>()
            let app = Self.allocatedSize(of: paths.installDirectory(for: id).path, counted: &counted)
            let data = [paths.dataDirectory(for: id), paths.writeBackDirectory(for: id),
                        paths.guestDataDirectory(for: id).deletingLastPathComponent(),
                        SessionLog(folder: paths.sessionLogDirectory).keptFile(for: id)]
                .reduce(0) { $0 + Self.allocatedSize(of: $1.path, counted: &counted) }
            return StorageUse(id: id, app: app, data: data)
        }
        return uses.enumerated().sorted { ($0.element.total, $1.offset) > ($1.element.total, $0.offset) }
            .map(\.element)
    }

    struct FileIdentity: Hashable {
        let device: dev_t
        let inode: ino_t
    }

    /// What the disk gives `path` and, for a folder, everything below it, read with lstat: a link counts as the link
    /// and is never followed, and a file linked twice counts once.
    static func allocatedSize(of path: String, counted: inout Set<FileIdentity>) -> UInt64 {
        var status = stat()
        guard lstat(path, &status) == 0,
              counted.insert(FileIdentity(device: status.st_dev, inode: status.st_ino)).inserted else { return 0 }
        var size = UInt64(status.st_blocks) * 512
        if status.st_mode & S_IFMT == S_IFDIR {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: path)) ?? [] {
                size += allocatedSize(of: path + "/" + name, counted: &counted)
            }
        }
        return size
    }
}
