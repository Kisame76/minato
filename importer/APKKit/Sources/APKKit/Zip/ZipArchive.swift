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
    /// The name's bytes as the archive holds them, which Android compares and looks up (libziparchive).
    public let nameBytes: Data
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
    /// Positions by the names' bytes: Android finds an entry by those, where Swift's String equality would make
    /// canonically equivalent names one.
    private let indexByName: [Data: Int]

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
        var index: [Data: Int] = [:]
        // libziparchive refuses a name twice, byte for byte (zip_cd_entry_map.cc:57-62, from zip_archive.cc:636-645):
        // two readers each taking another of them was the Master Key bug.
        for (position, entry) in entries.enumerated() {
            guard index.updateValue(position, forKey: entry.nameBytes) == nil else { throw APKError.duplicateEntry(entry.name) }
        }
        indexByName = index
    }

    public func entry(named name: String) -> ZipEntry? {
        indexByName[Data(name.utf8)].map { entries[$0] }
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
        let (content, header) = try read(entry, upTo: Int(entry.uncompressedSize))
        guard UInt64(content.count) == entry.uncompressedSize else {
            throw APKError.truncated("\(entry.name): got \(content.count) of \(entry.uncompressedSize) bytes")
        }
        guard CRC32.checksum(content) == entry.crc32 else {
            throw APKError.crcMismatch(entry.name)
        }
        try checkDataDescriptor(of: entry, header)
        return content
    }

    /// Hands an entry's uncompressed content to `body` in pieces of at most `chunkSize` bytes, so that
    /// no entry is ever in memory whole (ADR-0034, step d).
    public func forEachChunk(of entry: ZipEntry, chunkSize: Int = 1 << 20, _ body: (Data) throws -> Void) throws {
        guard !entry.isEncrypted else { throw APKError.encryptedEntry(entry.name) }
        let header = try localHeader(of: entry)
        let start = header.dataOffset
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
        try checkDataDescriptor(of: entry, header)
    }

    /// The first `count` bytes of an entry (fewer if the entry is shorter). Inflates only what is needed.
    public func prefix(of entry: ZipEntry, count: Int) throws -> Data {
        try read(entry, upTo: Int(min(UInt64(max(count, 0)), entry.uncompressedSize))).data
    }

    /// Absolute offset of the entry's (possibly compressed) data, after the local file header.
    public func dataOffset(of entry: ZipEntry) throws -> UInt64 {
        try localHeader(of: entry).dataOffset
    }

    // MARK: - Local headers, as libziparchive checks them when it finds an entry

    /// What an entry's checked local file header gives.
    private struct LocalHeader {
        var dataOffset: UInt64
        var hasDataDescriptor: Bool
    }

    /// The local file header of `entry` (zip_archive.cc:866-1033): its name is the central directory's, it and its
    /// extra field end before the central directory, its sizes and CRC-32 are the central directory's unless a data
    /// descriptor follows (ZIP64 sizes both at once), and the data ends before the central directory.
    private func localHeader(of entry: ZipEntry) throws -> LocalHeader {
        let directory = centralDirectoryOffset
        // :850-856: the header itself ends before the central directory, as a name follows it.
        guard entry.localHeaderOffset + 30 < directory else {
            throw APKError.inconsistentEntry(entry.name, "its local header runs into the central directory")
        }
        var header = ByteReader(try source.read(at: entry.localHeaderOffset, count: 30), context: entry.name)
        guard try header.u32() == 0x0403_4b50 else {
            throw APKError.truncated("\(entry.name): missing local file header")
        }
        let flags = try header.u16(at: 6)
        let crc = try header.u32(at: 14)
        var compressed = UInt64(try header.u32(at: 18))
        var uncompressed = UInt64(try header.u32(at: 22))
        let nameLength = UInt64(try header.u16(at: 26))
        let extraLength = UInt64(try header.u16(at: 28))

        // :873-912.
        let nameOffset = entry.localHeaderOffset + 30
        guard nameLength == UInt64(entry.nameBytes.count) else {
            throw APKError.inconsistentEntry(entry.name, "its local header names another file")
        }
        guard nameOffset + nameLength <= directory else {
            throw APKError.inconsistentEntry(entry.name, "its local header runs into the central directory")
        }
        guard try source.read(at: nameOffset, count: Int(nameLength)) == entry.nameBytes else {
            throw APKError.inconsistentEntry(entry.name, "its local header names another file")
        }
        // :913-919.
        let extraOffset = nameOffset + nameLength
        guard extraLength <= directory - extraOffset else {
            throw APKError.inconsistentEntry(entry.name, "its local header runs into the central directory")
        }
        // :927-961.
        if compressed == 0xFFFF_FFFF || uncompressed == 0xFFFF_FFFF {
            guard compressed == 0xFFFF_FFFF, uncompressed == 0xFFFF_FFFF else {
                throw APKError.inconsistentEntry(entry.name, "its local header gives one size in ZIP64")
            }
            var offset: UInt64 = 0
            try Self.applyZip64Extra(try source.read(at: extraOffset, count: Int(extraLength)),
                                     uncompressed: &uncompressed, compressed: &compressed, offset: &offset)
        }
        // :986-996; a mismatch of the descriptor bit alone is a warning there (:979-983).
        let hasDataDescriptor = flags & 0x0008 != 0
        if !hasDataDescriptor,
           compressed != entry.compressedSize || uncompressed != entry.uncompressedSize || crc != entry.crc32 {
            throw APKError.inconsistentEntry(entry.name, "its local header's sizes or CRC-32 differ")
        }
        // :1014-1033; the data's start lies before the central directory, as the extra field ends there.
        let dataOffset = extraOffset + extraLength
        guard entry.compressedSize <= directory - dataOffset,
              entry.method != .stored || entry.uncompressedSize <= directory - dataOffset else {
            throw APKError.inconsistentEntry(entry.name, "its data runs into the central directory")
        }
        return LocalHeader(dataOffset: dataOffset, hasDataDescriptor: hasDataDescriptor)
    }

    /// The data descriptor after an entry read whole repeats the central directory's CRC-32 and sizes, 64-bit where
    /// the entry needs them, after an optional signature (zip_archive.cc:735-785, from :1618-1619).
    private func checkDataDescriptor(of entry: ZipEntry, _ header: LocalHeader) throws {
        guard header.hasDataDescriptor else { return }
        let offset = header.dataOffset + (entry.method == .stored ? entry.uncompressedSize : entry.compressedSize)
        var reader = ByteReader(try source.read(at: offset, count: 24), context: entry.name)
        if try reader.u32(at: 0) == 0x0807_4b50 { try reader.skip(4) }
        let crc = try reader.u32()
        let wide = entry.compressedSize >= 0xFFFF_FFFF || entry.uncompressedSize >= 0xFFFF_FFFF
        let compressed = wide ? try reader.u64() : UInt64(try reader.u32())
        let uncompressed = wide ? try reader.u64() : UInt64(try reader.u32())
        guard crc == entry.crc32, compressed == entry.compressedSize, uncompressed == entry.uncompressedSize else {
            throw APKError.inconsistentEntry(entry.name, "its data descriptor differs")
        }
    }

    // MARK: - Entry data

    private func read(_ entry: ZipEntry, upTo target: Int) throws -> (data: Data, header: LocalHeader) {
        guard !entry.isEncrypted else { throw APKError.encryptedEntry(entry.name) }
        let header = try localHeader(of: entry)
        switch entry.method {
        case .stored:
            return (try source.read(at: header.dataOffset, count: Int(min(UInt64(target), entry.compressedSize))), header)
        case .deflated:
            return (try inflate(entry, from: header.dataOffset, upTo: target), header)
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

        // libziparchive walks exactly the records the end record counts (zip_archive.cc:558-573), each with its
        // local header before the central directory (:610-614) and a name it can read (:616-620).
        for index in 0..<end.entryCount {
            guard reader.remaining >= 46 else {
                throw APKError.truncated("central directory: \(entries.count) of \(end.entryCount) entries")
            }
            guard try reader.u32() == 0x0201_4b50 else {
                throw APKError.truncated("central directory: entry \(index) has no signature")
            }
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

            let name = decodeName(nameBytes, flags: flags)
            guard localHeaderOffset < end.centralDirectoryOffset else {
                throw APKError.inconsistentEntry(name, "its local header lies at or past the central directory")
            }
            guard isValidEntryName(nameBytes) else { throw APKError.invalidEntryName(name) }
            entries.append(ZipEntry(
                name: name,
                nameBytes: nameBytes,
                method: method == 0 ? .stored : method == 8 ? .deflated : .other(method),
                flags: flags,
                crc32: crc,
                compressedSize: compressedSize,
                uncompressedSize: uncompressedSize,
                localHeaderOffset: localHeaderOffset))
        }
        return entries
    }

    /// libziparchive's IsValidEntryName (entry_name_utils-inl.h:28-63): no NUL, and each sequence a lead byte other
    /// than 0xFE and 0xFF with as many continuation bytes as its high bits ask; an overlong form passes.
    static func isValidEntryName(_ name: Data) -> Bool {
        let bytes = Array(name)
        var index = 0
        while index < bytes.count {
            let byte = bytes[index]
            if byte == 0 || byte & 0xC0 == 0x80 || byte & 0xFE == 0xFE { return false }
            var lead = (byte & 0x7F) << 1
            while byte & 0x80 != 0, lead & 0x80 != 0 {
                index += 1
                guard index < bytes.count, bytes[index] & 0xC0 == 0x80 else { return false }
                lead = (lead & 0x7F) << 1
            }
            index += 1
        }
        return true
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
