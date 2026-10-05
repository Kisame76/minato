import APKKit
import Foundation
import zlib

extension Container {
    /// One entry to take out, under a file name of ours (ADR-0049 decision 6).
    public struct Item: Sendable, Equatable {
        public var entry: ZipEntry
        public var fileName: String

        public init(entry: ZipEntry, fileName: String) {
            self.entry = entry
            self.fileName = fileName
        }
    }

    /// The free space `extract` asks for: the declared sizes and a tenth more [guess], all of it where they overflow.
    public static func neededSpace(forSizes sizes: [UInt64]) -> UInt64 {
        var total: UInt64 = 0
        for size in sizes {
            let (sum, overflow) = total.addingReportingOverflow(size)
            guard !overflow else { return .max }
            total = sum
        }
        let (needed, overflow) = total.addingReportingOverflow(total / 10)
        return overflow ? .max : needed
    }

    /// The free space on `folder`'s volume for data the user asked for, as iOS reports it to an app.
    public static func availableSpace(at folder: URL) throws -> UInt64 {
        let values = try folder.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey,
                                                         .volumeAvailableCapacityKey])
        if let important = values.volumeAvailableCapacityForImportantUsage { return UInt64(max(important, 0)) }
        return UInt64(max(values.volumeAvailableCapacity ?? 0, 0))
    }

    /// Takes `items` out into the new folder `destination`, each chunk by chunk under its own name, never an entry's
    /// path (decision 6). The reader refuses more or less data than an entry declares, so a deflate bomb costs no
    /// more than its declared size, and each file's CRC-32 must match its entry's: Minato's own check, as Android
    /// checks none (`kCrcChecksEnabled`, zip_archive.cc:71). The free space is checked first, against the declared
    /// sizes and a tenth more. The folder is written as `<destination>.partial`, excluded from backups, and renamed
    /// when complete, so it exists whole or not at all: a failure removes it, and one an earlier run left goes
    /// first. An existing `destination` is never replaced.
    @discardableResult
    public func extract(_ items: [Item], into destination: URL,
                        availableSpace: (URL) throws -> UInt64 = Container.availableSpace) throws -> [URL] {
        var names = Set<String>()
        for item in items {
            guard Self.isPlainFileName(item.fileName), names.insert(item.fileName).inserted else {
                throw ContainerError.unsafeName(item.fileName)
            }
        }
        let fileManager = FileManager.default
        guard !fileManager.fileExists(atPath: destination.path) else { throw ContainerError.destinationExists }
        let parent = destination.deletingLastPathComponent()
        let needed = Self.neededSpace(forSizes: items.map(\.entry.uncompressedSize))
        let available = try availableSpace(parent)
        guard needed <= available else { throw ContainerError.notEnoughSpace(needed: needed, available: available) }

        var partial = parent.appendingPathComponent(destination.lastPathComponent + ".partial", isDirectory: true)
        try? fileManager.removeItem(at: partial)
        try fileManager.createDirectory(at: partial, withIntermediateDirectories: false)
        do {
            var excluded = URLResourceValues()
            excluded.isExcludedFromBackup = true
            try partial.setResourceValues(excluded)
            for item in items {
                try write(item.entry, to: partial.appendingPathComponent(item.fileName))
            }
            try fileManager.moveItem(at: partial, to: destination)
        } catch {
            try? fileManager.removeItem(at: partial)
            throw error
        }
        return items.map { destination.appendingPathComponent($0.fileName) }
    }

    /// One entry, chunk by chunk, with its CRC-32 checked and the file flushed to disk before the folder's rename.
    private func write(_ entry: ZipEntry, to file: URL) throws {
        guard FileManager.default.createFile(atPath: file.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: file.path])
        }
        let handle = try FileHandle(forWritingTo: file)
        defer { try? handle.close() }
        var crc = crc32(0, nil, 0)
        do {
            try archive.forEachChunk(of: entry) { chunk in
                chunk.withUnsafeBytes { raw in
                    crc = crc32(crc, raw.bindMemory(to: Bytef.self).baseAddress, uInt(raw.count))
                }
                try handle.write(contentsOf: chunk)
            }
        } catch let error as APKError {
            throw ContainerError.unreadable(error)
        }
        guard UInt32(truncatingIfNeeded: crc) == entry.crc32 else { throw ContainerError.checksumMismatch(entry.name) }
        try handle.synchronize()
    }

    /// A name for a file in one folder: not empty, not hidden (".", ".." among them), without a slash or a NUL, and
    /// no longer than a file name may be.
    static func isPlainFileName(_ name: String) -> Bool {
        !name.isEmpty && !name.hasPrefix(".") && !name.contains("/") && !name.contains("\0") && name.utf8.count <= 255
    }
}
