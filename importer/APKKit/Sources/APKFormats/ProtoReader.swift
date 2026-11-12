import Foundation

/// Why bytes are no protobuf message Minato reads.
public enum ProtoError: Error, Sendable, Equatable {
    /// Bytes end inside a field.
    case truncated(String)
    case invalid(String)
}

/// A reader of protobuf's wire format, as far as bundletool's table of contents needs it (ADR-0049 decision 2):
/// varints, fixed 32- and 64-bit values and length-delimited fields; a decoder skips the fields it does not know.
/// Groups are refused, as proto3 writes none. Every length is checked against what is left, a varint has at most
/// 10 bytes and no bit past 64, and messages nest at most `maxDepth` levels (decision 7).
public struct ProtoReader {
    public enum Value: Sendable, Equatable {
        case varint(UInt64)
        case fixed64(UInt64)
        case bytes(Data)
        case fixed32(UInt32)
    }

    public struct Field: Sendable, Equatable {
        public var number: Int
        public var value: Value

        public init(number: Int, value: Value) {
            self.number = number
            self.value = value
        }
    }

    /// [guess] Decision 7's cap; bundletool's deepest field, a toc's density, is 6 levels down.
    public static let maxDepth = 32
    /// protobuf's largest field number.
    static let maxFieldNumber: UInt64 = (1 << 29) - 1

    /// The level of this message below the outermost one, which is 0.
    public let depth: Int
    /// The message's bytes, which a field's bytes are a slice of, never a copy: a nested message shares its parent's
    /// storage, so a toc costs its size once, whatever its depth.
    private let bytes: Data
    /// An index into `bytes`, which a slice keeps from its parent.
    private var position: Int

    public init(_ data: Data, depth: Int = 0) throws {
        guard depth <= Self.maxDepth else { throw ProtoError.invalid("a message nested more than \(Self.maxDepth) levels") }
        bytes = data
        position = data.startIndex
        self.depth = depth
    }

    /// The next field, or nil at the message's end.
    public mutating func next() throws -> Field? {
        guard position < bytes.endIndex else { return nil }
        let key = try varint()
        let number = key >> 3
        guard number != 0, number <= Self.maxFieldNumber else { throw ProtoError.invalid("field number \(number)") }
        let field = Int(number)
        switch key & 7 {
        case 0: return Field(number: field, value: .varint(try varint()))
        case 1: return Field(number: field, value: .fixed64(try littleEndian(8, of: field)))
        case 2:
            let length = try varint()
            guard length <= UInt64(bytes.endIndex - position) else { throw ProtoError.truncated("field \(field)'s \(length) bytes") }
            return Field(number: field, value: .bytes(try take(Int(length), of: field)))
        case 5: return Field(number: field, value: .fixed32(UInt32(try littleEndian(4, of: field))))
        case let type: throw ProtoError.invalid("wire type \(type) of field \(field)")
        }
    }

    /// A field's message, one level further down.
    public func message(_ data: Data) throws -> ProtoReader {
        try ProtoReader(data, depth: depth + 1)
    }

    private mutating func varint() throws -> UInt64 {
        var result: UInt64 = 0
        for index in 0..<10 {
            guard position < bytes.endIndex else { throw ProtoError.truncated("a varint") }
            let byte = bytes[position]
            position += 1
            if index == 9, byte > 1 {
                throw ProtoError.invalid(byte & 0x80 != 0 ? "a varint of more than 10 bytes" : "a varint past 64 bits")
            }
            result |= UInt64(byte & 0x7F) << (7 * index)
            if byte & 0x80 == 0 { return result }
        }
        throw ProtoError.invalid("a varint of more than 10 bytes")
    }

    private mutating func littleEndian(_ count: Int, of field: Int) throws -> UInt64 {
        try take(count, of: field).reversed().reduce(0) { $0 << 8 | UInt64($1) }
    }

    /// The next `count` bytes as a slice of the message's.
    private mutating func take(_ count: Int, of field: Int) throws -> Data {
        guard count <= bytes.endIndex - position else { throw ProtoError.truncated("field \(field)'s \(count) bytes") }
        defer { position += count }
        return bytes[position..<position + count]
    }
}

extension ProtoReader.Value {
    public func bool() throws -> Bool { try varint() != 0 }

    /// An int32 or an enum: protobuf writes a negative one sign-extended to 64 bits and reads the low 32.
    public func int32() throws -> Int32 { Int32(truncatingIfNeeded: try varint()) }

    public func varint() throws -> UInt64 {
        switch self {
        case .varint(let value): return value
        case .bytes: throw ProtoError.invalid("a length-delimited field where a varint belongs")
        case .fixed32, .fixed64: throw ProtoError.invalid("a fixed field where a varint belongs")
        }
    }

    /// proto3's strings are UTF-8, as protobuf's own readers insist.
    public func string() throws -> String {
        guard let text = String(data: try data(), encoding: .utf8) else { throw ProtoError.invalid("a string that is no UTF-8") }
        return text
    }

    public func data() throws -> Data {
        switch self {
        case .bytes(let data): return data
        case .varint: throw ProtoError.invalid("a varint where a length-delimited field belongs")
        case .fixed32, .fixed64: throw ProtoError.invalid("a fixed field where a length-delimited field belongs")
        }
    }
}
