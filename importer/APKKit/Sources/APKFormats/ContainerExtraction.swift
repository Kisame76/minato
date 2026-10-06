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

    /// One entry, chunk by chunk, with its CRC-32 checked and the file flushed to disk before the folder's rename. A
    /// chunk of zeros is skipped, not written, so it stays a hole of a sparse file and an OBB's zeros cost no disk
    /// (ADR-0049 c1). The checksum still runs over it, and the file's length is set at the end, so it ends at the
    /// declared size also after a last chunk of zeros. The file is made new or not at all, so two names the volume takes
    /// for one, as APFS folds case on the Mac, fail closed (the lead's reviewer).
    private func write(_ entry: ZipEntry, to file: URL) throws {
        let descriptor = Darwin.open(file.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o644)
        guard descriptor >= 0 else {
            if errno == EEXIST { throw ContainerError.unsafeName(file.lastPathComponent) }
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: false)
        defer { try? handle.close() }
        var crc: UInt32 = 0
        var length: UInt64 = 0
        do {
            try archive.forEachChunk(of: entry) { chunk in
                crc = chunk.withUnsafeBytes { Self.checksum(crc, adding: $0) }
                if !Self.isZeros(chunk) {
                    try handle.seek(toOffset: length)
                    try handle.write(contentsOf: chunk)
                }
                length += UInt64(chunk.count)
            }
        } catch let error as APKError {
            throw ContainerError.unreadable(error)
        }
        guard crc == entry.crc32 else { throw ContainerError.checksumMismatch(entry.name) }
        try handle.truncate(atOffset: length)
        try handle.synchronize()
    }

    /// Whether `chunk` holds only zeros, compared with a block of zeros at a time.
    static func isZeros(_ chunk: Data) -> Bool {
        chunk.withUnsafeBytes { bytes in
            zeros.withUnsafeBytes { zeros in
                var offset = 0
                while offset < bytes.count {
                    let count = min(zeros.count, bytes.count - offset)
                    guard memcmp(bytes.baseAddress! + offset, zeros.baseAddress!, count) == 0 else { return false }
                    offset += count
                }
                return true
            }
        }
    }

    private static let zeros = [UInt8](repeating: 0, count: 64 << 10)

    /// The running CRC-32 `crc` with `bytes` added, through the system zlib. No bytes change nothing: zlib's crc32
    /// given no buffer answers its initial value instead (the lead's review of a1).
    public static func checksum(_ crc: UInt32, adding bytes: UnsafeRawBufferPointer) -> UInt32 {
        guard bytes.count > 0 else { return crc }
        return UInt32(truncatingIfNeeded: crc32(uLong(crc), bytes.bindMemory(to: Bytef.self).baseAddress, uInt(bytes.count)))
    }

    /// A name for a file in one folder: not empty, not hidden (".", ".." among them), without a slash or a NUL, and
    /// no longer than a file name may be. Checked on its UTF-8 bytes, as the file system reads it, so a combining mark
    /// hides neither a slash nor a leading dot (the lead's reviewer).
    static func isPlainFileName(_ name: String) -> Bool {
        let bytes = name.utf8
        return !bytes.isEmpty && bytes.first != UInt8(ascii: ".") && !bytes.contains(UInt8(ascii: "/")) && !bytes.contains(0)
            && bytes.count <= 255
    }
}
