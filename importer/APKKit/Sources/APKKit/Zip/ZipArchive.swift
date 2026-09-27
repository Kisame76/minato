import Compression
import Foundation

/// One file in a ZIP central directory.
public struct ZipEntry: Sendable, Hashable {
    public enum Method: Sendable, Hashable {
        case stored
        case deflated
        case other(UInt16)
    }

    public let name: String
    public let method: Method
    public let flags: UInt16
    public let crc32: UInt32
    public let compressedSize: UInt64
    public let uncompressedSize: UInt64
    public let localHeaderOffset: UInt64

    public var isDirectory: Bool { name.hasSuffix("/") }
    public var isEncrypted: Bool { flags & 0x0001 != 0 }
}

/// Read-only ZIP reader (PKWARE APPNOTE 6.3) with ZIP64 support.
///
/// Only the central directory is read up front; entry data is read on demand, so multi-gigabyte
/// APKs cost little memory. Not thread-safe.
public final class ZipArchive {
    public let entries: [ZipEntry]
    /// Start of the central directory. The APK Signing Block ends right before it.
    public let centralDirectoryOffset: UInt64
    public let centralDirectorySize: UInt64
    /// Where the end of central directory record starts (the classic one, after any ZIP64 records).
    public let endOfCentralDirectoryOffset: UInt64
    public let source: ByteSource
    private let indexByName: [String: Int]

    public static let defaultMaxEntrySize: UInt64 = 256 << 20

    public convenience init(data: Data) throws {
        try self.init(source: DataSource(data))
    }

    public convenience init(url: URL) throws {
        try self.init(source: FileSource(url: url))
    }

    public init(source: ByteSource) throws {
        self.source = source
        let end = try Self.readEndOfCentralDirectory(source)
        centralDirectoryOffset = end.centralDirectoryOffset
        centralDirectorySize = end.centralDirectorySize
        endOfCentralDirectoryOffset = end.offset
        entries = try Self.readCentralDirectory(source, end: end)
        var index: [String: Int] = [:]
        for (position, entry) in entries.enumerated() where index[entry.name] == nil {
            index[entry.name] = position
        }
        indexByName = index
    }

    public func entry(named name: String) -> ZipEntry? {
        indexByName[name].map { entries[$0] }
    }

    public func data(named name: String, maxSize: UInt64 = ZipArchive.defaultMaxEntrySize) throws -> Data {
        guard let entry = entry(named: name) else { throw APKError.entryNotFound(name) }
        return try data(for: entry, maxSize: maxSize)
    }

    /// Complete, CRC-checked content of an entry.
    public func data(for entry: ZipEntry, maxSize: UInt64 = ZipArchive.defaultMaxEntrySize) throws -> Data {
        guard entry.uncompressedSize <= maxSize else {
            throw APKError.entryTooLarge(entry.name, entry.uncompressedSize)
        }
        let content = try read(entry, upTo: Int(entry.uncompressedSize))
        guard UInt64(content.count) == entry.uncompressedSize else {
            throw APKError.truncated("\(entry.name): got \(content.count) of \(entry.uncompressedSize) bytes")
        }
        guard CRC32.checksum(content) == entry.crc32 else {
            throw APKError.crcMismatch(entry.name)
        }
        return content
    }

    /// Hands an entry's uncompressed content to `body` in pieces of at most `chunkSize` bytes, so that
    /// no entry is ever in memory whole (ADR-0034, step d).
    public func forEachChunk(of entry: ZipEntry, chunkSize: Int = 1 << 20, _ body: (Data) throws -> Void) throws {
        guard !entry.isEncrypted else { throw APKError.encryptedEntry(entry.name) }
        let start = try dataOffset(of: entry)
        guard entry.compressedSize <= source.size, start <= source.size - entry.compressedSize else {
            throw APKError.truncated("\(entry.name): data beyond end of archive")
        }
        var produced: UInt64 = 0
        // More than the entry's size is refused as it comes, so a deflate bomb costs no more than its size.
        func emit(_ piece: Data) throws {
            produced += UInt64(piece.count)
            guard produced <= entry.uncompressedSize else {
                throw APKError.truncated("\(entry.name): more data than its size of \(entry.uncompressedSize) bytes")
            }
            try body(piece)
        }
        switch entry.method {
        case .stored:
            var offset: UInt64 = 0
            while offset < entry.compressedSize {
                let count = Int(min(UInt64(chunkSize), entry.compressedSize - offset))
                try emit(try source.read(at: start + offset, count: count))
                offset += UInt64(count)
            }
        case .deflated:
            try inflate(entry, from: start, chunkSize: chunkSize, emit)
        case .other(let method):
            throw APKError.unsupportedCompression(entry: entry.name, method: method)
        }
        guard produced == entry.uncompressedSize else {
            throw APKError.truncated("\(entry.name): got \(produced) of \(entry.uncompressedSize) bytes")
        }
    }

    /// The first `count` bytes of an entry (fewer if the entry is shorter). Inflates only what is needed.
    public func prefix(of entry: ZipEntry, count: Int) throws -> Data {
        try read(entry, upTo: Int(min(UInt64(max(count, 0)), entry.uncompressedSize)))
    }

    /// Absolute offset of the entry's (possibly compressed) data, after the local file header.
    public func dataOffset(of entry: ZipEntry) throws -> UInt64 {
        var header = ByteReader(try source.read(at: entry.localHeaderOffset, count: 30), context: entry.name)
        guard try header.u32() == 0x0403_4b50 else {
            throw APKError.truncated("\(entry.name): missing local file header")
        }
        let nameLength = try header.u16(at: 26)
        let extraLength = try header.u16(at: 28)
        return entry.localHeaderOffset + 30 + UInt64(nameLength) + UInt64(extraLength)
    }

    // MARK: - Entry data

    private func read(_ entry: ZipEntry, upTo target: Int) throws -> Data {
        guard !entry.isEncrypted else { throw APKError.encryptedEntry(entry.name) }
        let start = try dataOffset(of: entry)
        guard entry.compressedSize <= source.size, start <= source.size - entry.compressedSize else {
            throw APKError.truncated("\(entry.name): data beyond end of archive")
        }
        switch entry.method {
        case .stored:
            return try source.read(at: start, count: Int(min(UInt64(target), entry.compressedSize)))
        case .deflated:
            return try inflate(entry, from: start, upTo: target)
        case .other(let method):
            throw APKError.unsupportedCompression(entry: entry.name, method: method)
        }
    }

    private func inflate(_ entry: ZipEntry, from start: UInt64, upTo target: Int) throws -> Data {
        guard target > 0 else { return Data() }
        let chunkSize = 64 * 1024
        let output = UnsafeMutablePointer<UInt8>.allocate(capacity: target)
        let input = UnsafeMutablePointer<UInt8>.allocate(capacity: chunkSize)
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer {
            output.deallocate()
            input.deallocate()
            stream.deallocate()
        }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw APKError.truncated("\(entry.name): cannot initialise inflater")
        }
        defer { compression_stream_destroy(stream) }

        stream.pointee.dst_ptr = output
        stream.pointee.dst_size = target
        stream.pointee.src_ptr = UnsafePointer(input)
        stream.pointee.src_size = 0
        var consumed: UInt64 = 0

        while stream.pointee.dst_size > 0 {
            if stream.pointee.src_size == 0 && consumed < entry.compressedSize {
                let count = Int(min(UInt64(chunkSize), entry.compressedSize - consumed))
                let chunk = try source.read(at: start + consumed, count: count)
                chunk.copyBytes(to: input, count: count)
                consumed += UInt64(count)
                stream.pointee.src_ptr = UnsafePointer(input)
                stream.pointee.src_size = count
            }
            let finalize = consumed >= entry.compressedSize
            let spaceBefore = stream.pointee.dst_size
            let status = compression_stream_process(stream, finalize ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0)
            if status == COMPRESSION_STATUS_END { break }
            guard status == COMPRESSION_STATUS_OK else {
                throw APKError.truncated("\(entry.name): invalid deflate data")
            }
            if finalize && stream.pointee.src_size == 0 && stream.pointee.dst_size == spaceBefore { break }
        }
        return Data(bytes: output, count: target - stream.pointee.dst_size)
    }

    /// Inflates the whole entry, handing the output to `body` whenever `chunkSize` bytes are ready.
    private func inflate(_ entry: ZipEntry, from start: UInt64, chunkSize: Int, _ body: (Data) throws -> Void) throws {
        let inputSize = 64 * 1024
        let output = UnsafeMutablePointer<UInt8>.allocate(capacity: chunkSize)
        let input = UnsafeMutablePointer<UInt8>.allocate(capacity: inputSize)
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer {
            output.deallocate()
            input.deallocate()
            stream.deallocate()
        }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw APKError.truncated("\(entry.name): cannot initialise inflater")
        }
        defer { compression_stream_destroy(stream) }

        stream.pointee.dst_ptr = output
        stream.pointee.dst_size = chunkSize
        stream.pointee.src_ptr = UnsafePointer(input)
        stream.pointee.src_size = 0
        var consumed: UInt64 = 0
        while true {
            if stream.pointee.src_size == 0 && consumed < entry.compressedSize {
                let count = Int(min(UInt64(inputSize), entry.compressedSize - consumed))
                let chunk = try source.read(at: start + consumed, count: count)
                chunk.copyBytes(to: input, count: count)
                consumed += UInt64(count)
                stream.pointee.src_ptr = UnsafePointer(input)
                stream.pointee.src_size = count
            }
            let finalize = consumed >= entry.compressedSize
            let sourceBefore = stream.pointee.src_size
            let spaceBefore = stream.pointee.dst_size
            let status = compression_stream_process(stream, finalize ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0)
            guard status == COMPRESSION_STATUS_OK || status == COMPRESSION_STATUS_END else {
                throw APKError.truncated("\(entry.name): invalid deflate data")
            }
            let filled = chunkSize - stream.pointee.dst_size
            if stream.pointee.dst_size == 0 || status == COMPRESSION_STATUS_END {
                if filled > 0 { try body(Data(bytes: output, count: filled)) }
                stream.pointee.dst_ptr = output
                stream.pointee.dst_size = chunkSize
            }
            if status == COMPRESSION_STATUS_END { return }
            if finalize && stream.pointee.src_size == sourceBefore && stream.pointee.dst_size == spaceBefore {
                throw APKError.truncated("\(entry.name): deflate data ends early")
            }
        }
    }

    // MARK: - Central directory

    struct EndRecord {
        var entryCount: UInt64
        var centralDirectorySize: UInt64
        var centralDirectoryOffset: UInt64
        var offset: UInt64
    }

    static func readEndOfCentralDirectory(_ source: ByteSource) throws -> EndRecord {
        let minimumSize: UInt64 = 22
        guard source.size >= minimumSize else { throw APKError.notAZipArchive }
        let tailLength = min(source.size, minimumSize + 0xFFFF)
        let tailStart = source.size - tailLength
        let tail = ByteReader(try source.read(at: tailStart, count: Int(tailLength)), context: "end of central directory")

        var position = tail.count - Int(minimumSize)
        while position >= 0 {
            if try tail.u32(at: position) == 0x0605_4b50 {
                let commentLength = Int(try tail.u16(at: position + 20))
                if position + Int(minimumSize) + commentLength <= tail.count { break }
            }
            position -= 1
        }
        guard position >= 0 else { throw APKError.notAZipArchive }

        let endOffset = tailStart + UInt64(position)
        var record = EndRecord(
            entryCount: UInt64(try tail.u16(at: position + 10)),
            centralDirectorySize: UInt64(try tail.u32(at: position + 12)),
            centralDirectoryOffset: UInt64(try tail.u32(at: position + 16)),
            offset: endOffset)

        // ZIP64: a locator directly precedes the classic end record.
        if endOffset >= 20 {
            let locator = ByteReader(try source.read(at: endOffset - 20, count: 20), context: "zip64 locator")
            if try locator.u32(at: 0) == 0x0706_4b50 {
                let zip64Offset = try locator.u64(at: 8)
                guard zip64Offset < endOffset else { throw APKError.notAZipArchive }
                let zip64 = ByteReader(try source.read(at: zip64Offset, count: 56), context: "zip64 end record")
                guard try zip64.u32(at: 0) == 0x0606_4b50 else { throw APKError.notAZipArchive }
                record.entryCount = try zip64.u64(at: 32)
                record.centralDirectorySize = try zip64.u64(at: 40)
                record.centralDirectoryOffset = try zip64.u64(at: 48)
            }
        }

        guard record.centralDirectoryOffset <= endOffset,
              record.centralDirectorySize <= endOffset - record.centralDirectoryOffset
        else { throw APKError.notAZipArchive }
        return record
    }

    static func readCentralDirectory(_ source: ByteSource, end: EndRecord) throws -> [ZipEntry] {
        guard end.centralDirectorySize <= 512 << 20 else {
            throw APKError.truncated("central directory of \(end.centralDirectorySize) bytes")
        }
        var reader = ByteReader(
            try source.read(at: end.centralDirectoryOffset, count: Int(end.centralDirectorySize)),
            context: "central directory")
        var entries: [ZipEntry] = []
        entries.reserveCapacity(Int(min(end.entryCount, 100_000)))

        while reader.remaining >= 46 {
            guard try reader.u32() == 0x0201_4b50 else { break }
            try reader.skip(4) // versions
            let flags = try reader.u16()
            let method = try reader.u16()
            try reader.skip(4) // time, date
            let crc = try reader.u32()
            var compressedSize = UInt64(try reader.u32())
            var uncompressedSize = UInt64(try reader.u32())
            let nameLength = Int(try reader.u16())
            let extraLength = Int(try reader.u16())
            let commentLength = Int(try reader.u16())
            try reader.skip(8) // disk number, internal and external attributes
            var localHeaderOffset = UInt64(try reader.u32())
            let nameBytes = try reader.bytes(nameLength)
            let extra = try reader.bytes(extraLength)
            try reader.skip(commentLength)

            if uncompressedSize == 0xFFFF_FFFF || compressedSize == 0xFFFF_FFFF || localHeaderOffset == 0xFFFF_FFFF {
                try applyZip64Extra(extra, uncompressed: &uncompressedSize, compressed: &compressedSize, offset: &localHeaderOffset)
            }

            entries.append(ZipEntry(
                name: decodeName(nameBytes, flags: flags),
                method: method == 0 ? .stored : method == 8 ? .deflated : .other(method),
                flags: flags,
                crc32: crc,
                compressedSize: compressedSize,
                uncompressedSize: uncompressedSize,
                localHeaderOffset: localHeaderOffset))
        }
        guard !entries.isEmpty || end.entryCount == 0 else { throw APKError.notAZipArchive }
        return entries
    }

    private static func applyZip64Extra(_ extra: Data, uncompressed: inout UInt64, compressed: inout UInt64, offset: inout UInt64) throws {
        var reader = ByteReader(extra, context: "zip64 extra field")
        while reader.remaining >= 4 {
            let id = try reader.u16()
            let size = Int(try reader.u16())
            let body = try reader.bytes(size)
            guard id == 0x0001 else { continue }
            var field = ByteReader(body, context: "zip64 extra field")
            if uncompressed == 0xFFFF_FFFF { uncompressed = try field.u64() }
            if compressed == 0xFFFF_FFFF { compressed = try field.u64() }
            if offset == 0xFFFF_FFFF { offset = try field.u64() }
            return
        }
        throw APKError.truncated("missing zip64 extra field")
    }

    private static func decodeName(_ bytes: Data, flags: UInt16) -> String {
        if let utf8 = String(data: bytes, encoding: .utf8) { return utf8 }
        return String(data: bytes, encoding: .isoLatin1) ?? ""
    }
}
