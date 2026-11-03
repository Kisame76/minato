import APKFormats
import Foundation
import Testing

/// Protobuf's wire format as far as bundletool's table of contents needs it (ADR-0049 decision 2), with its caps
/// (decision 7): every length checked against what is left, varints of at most 10 bytes, at most 32 levels.
@Suite("ProtoReader")
struct ProtoReaderTests {
    static func fields(_ bytes: [UInt8]) throws -> [ProtoReader.Field] {
        var reader = try ProtoReader(Data(bytes))
        var fields: [ProtoReader.Field] = []
        while let field = try reader.next() { fields.append(field) }
        return fields
    }

    @Test func readsEachWireTypeItTakes() throws {
        // Field 1 varint 150 (protobuf's own example), 2 fixed64, 3 bytes "hi", 5 fixed32.
        let fields = try Self.fields([0x08, 0x96, 0x01,
                                      0x11, 1, 2, 3, 4, 5, 6, 7, 8,
                                      0x1A, 0x02, 0x68, 0x69,
                                      0x2D, 9, 8, 7, 6])
        #expect(fields == [.init(number: 1, value: .varint(150)), .init(number: 2, value: .fixed64(0x0807_0605_0403_0201)),
                           .init(number: 3, value: .bytes(Data("hi".utf8))), .init(number: 5, value: .fixed32(0x0607_0809))])
    }

    @Test func readsTheLargestFieldNumberAndVarint() throws {
        // Field number 2^29 - 1 in a 5-byte key; 2^64 - 1 in 10 bytes.
        let fields = try Self.fields([0xF8, 0xFF, 0xFF, 0xFF, 0x0F,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01])
        #expect(fields == [.init(number: (1 << 29) - 1, value: .varint(.max))])
    }

    @Test(arguments: [
        ([0x08, 0x96], ProtoError.truncated("a varint")),
        ([0x08] + [UInt8](repeating: 0xFF, count: 10) + [0x01], ProtoError.invalid("a varint of more than 10 bytes")),
        ([0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02], ProtoError.invalid("a varint past 64 bits")),
        ([0x11, 1, 2, 3], ProtoError.truncated("field 2's 8 bytes")),
        ([0x11, 1, 2, 3, 4, 5, 6, 7], ProtoError.truncated("field 2's 8 bytes")),
        ([0x2D, 1, 2], ProtoError.truncated("field 5's 4 bytes")),
        ([0x2D, 1, 2, 3], ProtoError.truncated("field 5's 4 bytes")),
        ([0x1A, 0x03, 0x68, 0x69], ProtoError.truncated("field 3's 3 bytes")),
        ([0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F], ProtoError.truncated("field 3's 4294967295 bytes")),
        ([0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F], ProtoError.truncated("field 3's 9223372036854775807 bytes")),
        ([0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01],
         ProtoError.truncated("field 3's 18446744073709551615 bytes")),
        ([0x02, 0x00], ProtoError.invalid("field number 0")),
        ([0x80, 0x80, 0x80, 0x80, 0x10, 0x00], ProtoError.invalid("field number 536870912")),
        ([0x0B], ProtoError.invalid("wire type 3 of field 1")),
        ([0x0C], ProtoError.invalid("wire type 4 of field 1")),
        ([0x0E], ProtoError.invalid("wire type 6 of field 1")),
        ([0x0F], ProtoError.invalid("wire type 7 of field 1")),
    ] as [([UInt8], ProtoError)])
    func refusesWhatIsNoMessage(_ bytes: [UInt8], _ error: ProtoError) {
        #expect(throws: error) { try Self.fields(bytes) }
    }

    @Test func aMessageNestsAtMost32Levels() throws {
        // Field 1 holding field 1 holding ... an empty message at the bottom.
        func nested(_ levels: Int) -> Data {
            var data = Data()
            for _ in 0..<levels { data = Data([0x0A, UInt8(data.count)]) + data }
            return data
        }
        func depth(of data: Data, at level: Int = 0) throws -> Int {
            var reader = try ProtoReader(data, depth: level)
            guard let field = try reader.next(), case .bytes(let inner) = field.value else { return level }
            return try depth(of: inner, at: level + 1)
        }
        #expect(try depth(of: nested(32)) == 32)
        #expect(throws: ProtoError.invalid("a message nested more than 32 levels")) { try depth(of: nested(33)) }
    }

    @Test func aNestedMessageSharesItsParentsBytes() throws {
        // Each level reading a copy would cost a 16 MiB toc.pb its depth times 16 MiB (the review of a2):
        // field 1 holds field 2, which holds 200 bytes, and both point into the outer buffer.
        let payload = Data(repeating: 0x2A, count: 200)
        let middle = Data([0x12, 0xC8, 0x01]) + payload
        let outer = Data([0x0A, 0xCB, 0x01]) + middle
        var reader = try ProtoReader(outer)
        let first = try #require(try reader.next())
        guard case .bytes(let middleBytes) = first.value else { Issue.record("field 1 is no message"); return }
        var inner = try reader.message(middleBytes)
        let second = try #require(try inner.next())
        guard case .bytes(let payloadBytes) = second.value else { Issue.record("field 2 holds no bytes"); return }
        #expect(payloadBytes == payload)
        outer.withUnsafeBytes { outerBuffer in
            middleBytes.withUnsafeBytes { #expect($0.baseAddress == outerBuffer.baseAddress! + 3) }
            payloadBytes.withUnsafeBytes { #expect($0.baseAddress == outerBuffer.baseAddress! + 6) }
        }
    }

    @Test func readsTheValuesTheTocHolds() throws {
        #expect(try ProtoReader.Value.varint(1).bool() == true)
        #expect(try ProtoReader.Value.varint(0).bool() == false)
        // int32 as protobuf writes a negative one: sign-extended to 10 bytes; a larger value is cut to 32 bits.
        #expect(try ProtoReader.Value.varint(UInt64(bitPattern: -2)).int32() == -2)
        #expect(try ProtoReader.Value.varint(0x1_0000_0005).int32() == 5)
        #expect(try ProtoReader.Value.bytes(Data("dé".utf8)).string() == "dé")
        #expect(throws: ProtoError.invalid("a string that is no UTF-8")) { try ProtoReader.Value.bytes(Data([0xC3])).string() }
        #expect(throws: ProtoError.invalid("a length-delimited field where a varint belongs")) {
            try ProtoReader.Value.bytes(Data()).int32()
        }
        #expect(throws: ProtoError.invalid("a varint where a length-delimited field belongs")) {
            try ProtoReader.Value.varint(1).string()
        }
    }
}
