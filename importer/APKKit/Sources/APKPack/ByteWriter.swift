import Foundation

/// Little-endian byte buffer used by the test builders.
public struct ByteWriter {
    public private(set) var data = Data()

    public init() {}

    public var count: Int { data.count }

    public mutating func u8(_ value: UInt8) { data.append(value) }

    public mutating func u16(_ value: UInt16) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    public mutating func u32(_ value: UInt32) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    public mutating func u64(_ value: UInt64) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    public mutating func i32(_ value: Int32) { u32(UInt32(bitPattern: value)) }

    public mutating func bytes(_ value: Data) { data.append(value) }

    public mutating func bytes(_ value: [UInt8]) { data.append(contentsOf: value) }

    public mutating func zeros(_ count: Int) { data.append(Data(count: count)) }

    public mutating func pad(toMultipleOf alignment: Int) {
        let remainder = data.count % alignment
        if remainder != 0 { zeros(alignment - remainder) }
    }

    /// Overwrites a previously written 32-bit value, e.g. a chunk size.
    public mutating func patchU32(at offset: Int, _ value: UInt32) {
        withUnsafeBytes(of: value.littleEndian) { raw in
            data.replaceSubrange(offset..<offset + 4, with: raw)
        }
    }

    public mutating func patchU16(at offset: Int, _ value: UInt16) {
        withUnsafeBytes(of: value.littleEndian) { raw in
            data.replaceSubrange(offset..<offset + 2, with: raw)
        }
    }
}
