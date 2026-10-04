import Foundation

/// ZIPs that Android opens though one entry cannot be read, for the walks over a whole import (ADR-0048 b1.2).
public enum CraftedZIP {
    /// Where an extra entry's ZIP64 field puts its local header.
    public enum Offset: Sendable {
        /// A saturated 32-bit offset and its ZIP64 value: from 2^63 libziparchive's open reads it as a negative
        /// off64_t and passes it (zip_archive.cc:596-611), and reading the entry fails.
        case saturated(UInt64)
        /// A complete ZIP64 field, read for a saturated size, beside an unsaturated 32-bit offset: the open checks
        /// only the 32-bit one, and reading takes the ZIP64 one (:832-856).
        case complete(UInt64)

        /// One of each, past what an entry can be read at: 2^63 and near 2^64.
        public static let unreadable: [Offset] = [.saturated(1 << 63), .complete(.max - 5)]
    }

    /// `zip` with one more central-directory record, `name`, stored and empty, its local header where `offset` puts
    /// it. `zip` ends in its end record, without a comment or ZIP64 records, as `ZipBuilder` writes it unsigned.
    public static func adding(_ name: String, at offset: Offset, to zip: Data) -> Data {
        let bytes = [UInt8](zip)
        func u16(_ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }
        func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }
        let end = bytes.count - 22
        precondition(u32(end) == 0x0605_4b50 && u16(end + 20) == 0, "an end record without a comment")
        let count = u16(end + 10), size = u32(end + 12), directory = u32(end + 16)
        precondition(directory + size == end, "the end record right after the central directory")

        var extra = ByteWriter()
        let compressed32: UInt32, offset32: UInt32
        switch offset {
        case .saturated(let value):
            extra.u16(0x0001); extra.u16(8); extra.u64(value)
            (compressed32, offset32) = (0, .max)
        case .complete(let value):
            extra.u16(0x0001); extra.u16(24); extra.u64(0); extra.u64(0); extra.u64(value)
            (compressed32, offset32) = (.max, 0)
        }
        let nameBytes = Data(name.utf8)
        var record = ByteWriter()
        record.u32(0x0201_4b50); record.u16(20); record.u16(20); record.u16(0); record.u16(0); record.u16(0); record.u16(0)
        record.u32(0); record.u32(compressed32); record.u32(0)
        record.u16(UInt16(nameBytes.count)); record.u16(UInt16(extra.count)); record.u16(0); record.u16(0); record.u16(0)
        record.u32(0); record.u32(offset32); record.bytes(nameBytes); record.bytes(extra.data)

        var out = ByteWriter()
        out.bytes(zip.prefix(end)); out.bytes(record.data)
        out.u32(0x0605_4b50); out.u16(0); out.u16(0); out.u16(UInt16(count + 1)); out.u16(UInt16(count + 1))
        out.u32(UInt32(size + record.count)); out.u32(UInt32(directory)); out.u16(0)
        return out.data
    }
}
