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
    /// Why reading the entry fails, found when the archive opened; Android opens such an archive and fails there too.
    let readProblem: String?

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
        // zip_archive.cc:409-414.
        guard end.entryCount > 0 else { throw APKError.invalidArchive("it holds no entries") }
        entries = try Self.readCentralDirectory(source, end: end)
        // :649-663: the file starts with a local header, so nothing is prepended to it.
        guard try ByteReader(try source.read(at: 0, count: 4), context: "start").u32(at: 0) == 0x0403_4b50 else {
            throw APKError.invalidArchive("it does not start with a local header")
        }
        var index: [Data: Int] = [:]
        // libziparchive refuses a name twice, byte for byte (zip_cd_entry_map.cc:57-62, from zip_archive.cc:636-645):
        // two readers each taking another of them was the Master Key bug.
        for (position, entry) in entries.enumerated() {
            guard index.updateValue(position, forKey: entry.nameBytes) == nil else { throw APKError.duplicateEntry(entry.name) }
        }
        indexByName = index
    }

    /// The entry whose name is `name`'s UTF-8, byte for byte, as Android looks one up. An entry whose name is no UTF-8
    /// has a `name` starting with a NUL (`decodeName`), which finds nothing.
    public func entry(named name: String) -> ZipEntry? {
        indexByName[Data(name.utf8)].map { entries[$0] }
    }

    public func data(named name: String, maxSize: UInt64 = ZipArchive.defaultMaxEntrySize) throws -> Data {
        guard let entry = entry(named: name) else { throw APKError.entryNotFound(name) }
        return try data(for: entry, maxSize: maxSize)
    }

    /// Complete, CRC-checked content of an entry.
    public func data(for entry: ZipEntry, maxSize: UInt64 = ZipArchive.defaultMaxEntrySize) throws -> Data {
        // Its sizes may be the 32-bit ones a ZIP64 field Android cannot read left.
        if let problem = entry.readProblem { throw APKError.inconsistentEntry(entry.name, problem) }
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
        if let problem = entry.readProblem { throw APKError.inconsistentEntry(entry.name, problem) }
        let directory = centralDirectoryOffset
        // :850-856: the header itself ends before the central directory, as a name follows it; an offset near the top
        // overflows there.
        let (headerEnd, overflow) = entry.localHeaderOffset.addingReportingOverflow(30)
        guard !overflow, headerEnd < directory else {
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
            guard let values = Self.zip64Values(try source.read(at: extraOffset, count: Int(extraLength)),
                                                uncompressed: .max, compressed: .max, offset: nil),
                  let localUncompressed = values.uncompressed, let localCompressed = values.compressed else {
                throw APKError.inconsistentEntry(entry.name, "its local header's ZIP64 field does not hold its sizes")
            }
            (uncompressed, compressed) = (localUncompressed, localCompressed)
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

    /// The end record as libziparchive finds it (zip_archive.cc:278-346): the last signature in the file's tail,
    /// with nothing after it and its comment, and the ZIP64 end record only where one of its fields is saturated.
    static func readEndOfCentralDirectory(_ source: ByteSource) throws -> EndRecord {
        let minimumSize: UInt64 = 22
        guard source.size >= minimumSize else { throw APKError.notAZipArchive }
        let tailLength = min(source.size, minimumSize + 0xFFFF)
        let tailStart = source.size - tailLength
        let tail = ByteReader(try source.read(at: tailStart, count: Int(tailLength)), context: "end of central directory")

        var position = tail.count - Int(minimumSize)
        while position >= 0, try tail.u32(at: position) != 0x0605_4b50 {
            position -= 1
        }
        guard position >= 0 else { throw APKError.notAZipArchive }
        let endOffset = tailStart + UInt64(position)
        // :318-324.
        let commentLength = try tail.u16(at: position + 20)
        guard endOffset + minimumSize + UInt64(commentLength) == source.size else {
            throw APKError.invalidArchive("bytes follow its end record")
        }
        let recordsOnDisk = try tail.u16(at: position + 8), records = try tail.u16(at: position + 10)
        let size = try tail.u32(at: position + 12), offset = try tail.u32(at: position + 16)
        // :326-336.
        if recordsOnDisk == .max || records == .max || size == .max || offset == .max || commentLength == .max {
            return try readZip64EndOfCentralDirectory(source, endOffset: endOffset)
        }
        // :343-347.
        guard UInt64(offset) + UInt64(size) <= endOffset else { throw APKError.notAZipArchive }
        return EndRecord(entryCount: UInt64(records), centralDirectorySize: UInt64(size),
                         centralDirectoryOffset: UInt64(offset), offset: endOffset)
    }

    /// The ZIP64 end record its locator, right before the end record, names (zip_archive.cc:214-275).
    static func readZip64EndOfCentralDirectory(_ source: ByteSource, endOffset: UInt64) throws -> EndRecord {
        let missing = APKError.invalidArchive("its end record asks for ZIP64, which is missing")
        guard endOffset > 20 else { throw missing }
        let locatorOffset = endOffset - 20
        let locator = ByteReader(try source.read(at: locatorOffset, count: 20), context: "zip64 locator")
        guard try locator.u32(at: 0) == 0x0706_4b50 else { throw missing }
        let recordOffset = try locator.u64(at: 8)
        guard locatorOffset > 56, recordOffset <= locatorOffset - 56 else { throw APKError.notAZipArchive }
        let record = ByteReader(try source.read(at: recordOffset, count: 56), context: "zip64 end record")
        guard try record.u32(at: 0) == 0x0606_4b50 else { throw missing }
        let count = try record.u64(at: 32), size = try record.u64(at: 40), offset = try record.u64(at: 48)
        guard recordOffset > size, offset <= recordOffset - size else { throw APKError.notAZipArchive }
        return EndRecord(entryCount: count, centralDirectorySize: size, centralDirectoryOffset: offset, offset: endOffset)
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
            let compressed32 = try reader.u32()
            let uncompressed32 = try reader.u32()
            let nameLength = Int(try reader.u16())
            let extraLength = Int(try reader.u16())
            let commentLength = Int(try reader.u16())
            try reader.skip(8) // disk number, internal and external attributes
            let offset32 = try reader.u32()
            let nameBytes = try reader.bytes(nameLength)
            let extra = try reader.bytes(extraLength)
            try reader.skip(commentLength)
            let name = decodeName(nameBytes, flags: flags)

            // A saturated field takes its ZIP64 value, and a complete ZIP64 field all three, as reading the entry does
            // (zip_archive.cc:832-848); a field Android cannot read fails the entry then, or the open where the offset
            // needs it (:598-608).
            var uncompressedSize = UInt64(uncompressed32), compressedSize = UInt64(compressed32)
            var localHeaderOffset = UInt64(offset32)
            var readProblem: String?
            if uncompressed32 == .max || compressed32 == .max || offset32 == .max {
                if let values = zip64Values(extra, uncompressed: uncompressed32, compressed: compressed32, offset: offset32) {
                    uncompressedSize = values.uncompressed ?? uncompressedSize
                    compressedSize = values.compressed ?? compressedSize
                    localHeaderOffset = values.offset ?? localHeaderOffset
                } else if offset32 == .max {
                    throw APKError.inconsistentEntry(name, "its ZIP64 field does not hold its saturated values")
                } else {
                    readProblem = "its ZIP64 field does not hold its saturated values"
                }
            }
            // :610-614 checks the offset the open reads: the ZIP64 one only where the 32-bit one is saturated, as an
            // off64_t, so one from 2^63 is negative and passes; reading the entry fails then.
            let openOffset = offset32 == .max ? Int64(bitPattern: localHeaderOffset) : Int64(offset32)
            guard openOffset < Int64(bitPattern: end.centralDirectoryOffset) else {
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
                localHeaderOffset: localHeaderOffset,
                readProblem: readProblem))
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

    /// A ZIP64 extended information field's values as libziparchive's ParseZip64ExtendedInfoInExtraField reads them
    /// (zip_archive.cc:438-537): each value whose 32-bit field is saturated, or all three where the field is complete
    /// (24 bytes, or 28 with a disk number it ignores), and the field exactly as long as what was read. nil where
    /// Android fails.
    static func zip64Values(_ extra: Data, uncompressed: UInt32, compressed: UInt32, offset: UInt32?)
        -> (uncompressed: UInt64?, compressed: UInt64?, offset: UInt64?)? {
        let reader = ByteReader(extra, context: "zip64 extra field")
        var position = 0
        // A field of 4 bytes or fewer holds no header, which Android refuses too (:443-447).
        while position < reader.count - 4 {
            guard let id = try? reader.u16(at: position), var size = try? reader.u16(at: position + 2) else { return nil }
            position += 4
            // :459-462: every header's data lies within the field, before its ID counts or 28 bytes become 24.
            guard Int(size) <= reader.count - position else { return nil }
            guard id == 0x0001 else {
                position += Int(size)
                continue
            }
            if size == 28 { size = 24 }
            let complete = size == 24
            var cursor = position
            func next() -> UInt64? {
                defer { cursor += 8 }
                return try? reader.u64(at: cursor)
            }
            var values: (uncompressed: UInt64?, compressed: UInt64?, offset: UInt64?) = (nil, nil, nil)
            if uncompressed == .max || complete {
                guard let value = next() else { return nil }
                values.uncompressed = value
            }
            if compressed == .max || complete {
                guard let value = next() else { return nil }
                values.compressed = value
            }
            if offset == .max || complete {
                guard let value = next() else { return nil }
                values.offset = value
            }
            // At least one value was read, as a 32-bit field is saturated or the field complete.
            guard cursor - position == Int(size) else { return nil }
            return values
        }
        return nil
    }

    /// A name as Swift text: its UTF-8 exactly, a byte-order mark kept, which Foundation's decoding drops. A name
    /// Android accepts that is no UTF-8, an overlong form for one, reads as its bytes in Latin-1 after a NUL, which no
    /// name Android opens holds: `entry(named:)` never finds it, nor another entry for it.
    private static func decodeName(_ bytes: Data, flags: UInt16) -> String {
        let text = String(decoding: bytes, as: UTF8.self)
        if Data(text.utf8) == bytes { return text }
        return "\0" + (String(data: bytes, encoding: .isoLatin1) ?? "")
    }
}
