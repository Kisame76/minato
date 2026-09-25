import Compression
import Foundation

/// Builds ZIP archives byte by byte for tests (PKWARE APPNOTE layout).
public struct ZipBuilder {
    public enum Method {
        case stored
        case deflated
    }

    public struct Entry {
        public var name: String
        public var content: Data
        public var method: Method
        /// Pads the local header so that the entry data starts at a multiple of this value.
        public var alignment: Int?

        public init(name: String, content: Data, method: Method = .deflated, alignment: Int? = nil) {
            self.name = name
            self.content = content
            self.method = method
            self.alignment = alignment
        }
    }

    /// Result of `build()`, with offsets computed while writing.
    public struct Output {
        public var data: Data
        /// Offset of the first byte of each entry's (possibly compressed) data, keyed by name.
        public var dataOffsets: [String: Int]
        public var centralDirectoryOffset: Int
    }

    public var entries: [Entry] = []
    public var comment = Data()
    /// Writes ZIP64 end-of-central-directory records and ZIP64 extra fields.
    public var forceZip64 = false
    /// Bytes inserted between the last entry and the central directory
    /// (the position of the APK Signing Block).
    public var beforeCentralDirectory = Data()

    public init(entries: [Entry] = []) {
        self.entries = entries
    }

    public mutating func add(_ name: String, _ content: Data, method: Method = .deflated, alignment: Int? = nil) {
        entries.append(Entry(name: name, content: content, method: method, alignment: alignment))
    }

    public mutating func add(_ name: String, _ text: String, method: Method = .deflated) {
        add(name, Data(text.utf8), method: method)
    }

    public func build() -> Output {
        var out = ByteWriter()
        var dataOffsets: [String: Int] = [:]
        var central = ByteWriter()

        for entry in entries {
            let nameBytes = Data(entry.name.utf8)
            let payload = entry.method == .stored ? entry.content : Self.deflate(entry.content)
            let methodCode: UInt16 = entry.method == .stored ? 0 : 8
            let crc = CRC32Reference.checksum(entry.content)
            let headerOffset = out.count

            var localExtra = ByteWriter()
            if forceZip64 {
                localExtra.u16(0x0001)
                localExtra.u16(16)
                localExtra.u64(UInt64(entry.content.count))
                localExtra.u64(UInt64(payload.count))
            }
            if let alignment = entry.alignment {
                let unpadded = headerOffset + 30 + nameBytes.count + localExtra.count
                var padding = (alignment - unpadded % alignment) % alignment
                if padding > 0 && padding < 4 { padding += alignment }
                if padding > 0 {
                    // Android zipalign-style padding extra field (ID 0xD935).
                    localExtra.u16(0xD935)
                    localExtra.u16(UInt16(padding - 4))
                    localExtra.zeros(padding - 4)
                }
            }

            out.u32(0x0403_4b50)
            out.u16(forceZip64 ? 45 : 20)
            out.u16(0x0800) // UTF-8 names
            out.u16(methodCode)
            out.u16(0)
            out.u16(0x21)
            out.u32(crc)
            out.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(payload.count))
            out.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(entry.content.count))
            out.u16(UInt16(nameBytes.count))
            out.u16(UInt16(localExtra.count))
            out.bytes(nameBytes)
            out.bytes(localExtra.data)
            dataOffsets[entry.name] = out.count
            out.bytes(payload)

            var centralExtra = ByteWriter()
            if forceZip64 {
                centralExtra.u16(0x0001)
                centralExtra.u16(24)
                centralExtra.u64(UInt64(entry.content.count))
                centralExtra.u64(UInt64(payload.count))
                centralExtra.u64(UInt64(headerOffset))
            }
            central.u32(0x0201_4b50)
            central.u16(forceZip64 ? 45 : 20)
            central.u16(forceZip64 ? 45 : 20)
            central.u16(0x0800)
            central.u16(methodCode)
            central.u16(0)
            central.u16(0x21)
            central.u32(crc)
            central.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(payload.count))
            central.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(entry.content.count))
            central.u16(UInt16(nameBytes.count))
            central.u16(UInt16(centralExtra.count))
            central.u16(0)
            central.u16(0)
            central.u16(0)
            central.u32(0)
            central.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(headerOffset))
            central.bytes(nameBytes)
            central.bytes(centralExtra.data)
        }

        out.bytes(beforeCentralDirectory)
        let centralOffset = out.count
        out.bytes(central.data)

        if forceZip64 {
            let zip64RecordOffset = out.count
            out.u32(0x0606_4b50)
            out.u64(44)
            out.u16(45)
            out.u16(45)
            out.u32(0)
            out.u32(0)
            out.u64(UInt64(entries.count))
            out.u64(UInt64(entries.count))
            out.u64(UInt64(central.count))
            out.u64(UInt64(centralOffset))
            out.u32(0x0706_4b50)
            out.u32(0)
            out.u64(UInt64(zip64RecordOffset))
            out.u32(1)
        }

        out.u32(0x0605_4b50)
        out.u16(0)
        out.u16(0)
        out.u16(forceZip64 ? 0xFFFF : UInt16(entries.count))
        out.u16(forceZip64 ? 0xFFFF : UInt16(entries.count))
        out.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(central.count))
        out.u32(forceZip64 ? 0xFFFF_FFFF : UInt32(centralOffset))
        out.u16(UInt16(comment.count))
        out.bytes(comment)

        return Output(data: out.data, dataOffsets: dataOffsets, centralDirectoryOffset: centralOffset)
    }

    /// Raw DEFLATE (RFC 1951), which is what ZIP method 8 stores.
    public static func deflate(_ data: Data) -> Data {
        if data.isEmpty { return Data([0x03, 0x00]) }
        // Data that does not compress comes out a little larger: Apple's zlib stores it in blocks of
        // 16 KiB with 5 bytes each. A buffer that is too small makes the encoder return 0. An eighth
        // more is far beyond that overhead.
        let capacity = data.count + data.count / 8 + 1024
        var output = Data(count: capacity)
        let written = output.withUnsafeMutableBytes { dst in
            data.withUnsafeBytes { src in
                compression_encode_buffer(
                    dst.bindMemory(to: UInt8.self).baseAddress!, capacity,
                    src.bindMemory(to: UInt8.self).baseAddress!, data.count,
                    nil, COMPRESSION_ZLIB)
            }
        }
        precondition(written > 0, "deflate failed")
        return output.prefix(written)
    }
}

/// Bitwise CRC-32 (IEEE 802.3), deliberately independent of the production table implementation.
public enum CRC32Reference {
    public static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB8_8320 : crc >> 1
            }
        }
        return ~crc
    }
}
