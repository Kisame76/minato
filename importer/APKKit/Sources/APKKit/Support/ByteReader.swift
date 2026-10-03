import Foundation

/// Bounds-checked little-endian cursor over a byte buffer. Every read throws instead of trapping.
struct ByteReader {
    let data: Data
    private(set) var offset: Int
    private let context: String

    init(_ data: Data, offset: Int = 0, context: String = "data") {
        // Rebase so that indices start at zero, regardless of how the Data was sliced.
        self.data = data.startIndex == 0 ? data : Data(data)
        self.offset = offset
        self.context = context
    }

    var count: Int { data.count }
    var remaining: Int { max(0, data.count - offset) }

    mutating func seek(to position: Int) throws {
        guard position >= 0, position <= data.count else {
            throw APKError.truncated("\(context): seek to \(position) beyond \(data.count)")
        }
        offset = position
    }

    mutating func skip(_ count: Int) throws {
        try seek(to: offset + count)
    }

    mutating func u8() throws -> UInt8 { try UInt8(integer(1)) }
    mutating func u16() throws -> UInt16 { try UInt16(integer(2)) }
    mutating func u32() throws -> UInt32 { try UInt32(integer(4)) }
    mutating func u64() throws -> UInt64 { try integer(8) }
    mutating func i32() throws -> Int32 { Int32(bitPattern: try u32()) }

    mutating func bytes(_ count: Int) throws -> Data {
        guard count >= 0, count <= remaining else {
            throw APKError.truncated("\(context): need \(count) bytes at \(offset), have \(remaining)")
        }
        defer { offset += count }
        return data.subdata(in: offset..<offset + count)
    }

    /// Reads without moving the cursor.
    func u8(at position: Int) throws -> UInt8 { try UInt8(integer(1, at: position)) }
    func u16(at position: Int) throws -> UInt16 { try UInt16(integer(2, at: position)) }
    func u32(at position: Int) throws -> UInt32 { try UInt32(integer(4, at: position)) }
    func u64(at position: Int) throws -> UInt64 { try integer(8, at: position) }

    private mutating func integer(_ width: Int) throws -> UInt64 {
        let value = try integer(width, at: offset)
        offset += width
        return value
    }

    private func integer(_ width: Int, at position: Int) throws -> UInt64 {
        guard position >= 0, width <= data.count, position <= data.count - width else {
            throw APKError.truncated("\(context): need \(width) bytes at \(position), have \(data.count)")
        }
        var value: UInt64 = 0
        for index in 0..<width {
            value |= UInt64(data[position + index]) << (8 * UInt64(index))
        }
        return value
    }
}
