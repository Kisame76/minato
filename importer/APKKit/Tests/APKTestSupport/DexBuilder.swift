import Foundation

/// A dex file with only what DexStrings reads (dex_file.h): the header's string IDs and class definitions, one class
/// whose direct methods hold the given instructions, and the string data. Checksum and signature stay zero.
public enum DexBuilder {
    /// const-string vA, string@index (21c).
    public static func constString(_ register: UInt8, _ index: UInt16) -> [UInt16] {
        [UInt16(register) << 8 | 0x1A, index]
    }

    /// const-string/jumbo vA, string@index (31c).
    public static func constStringJumbo(_ register: UInt8, _ index: UInt32) -> [UInt16] {
        [UInt16(register) << 8 | 0x1B, UInt16(index & 0xFFFF), UInt16(index >> 16)]
    }

    public static let returnVoid: [UInt16] = [0x000E]

    /// A fill-array-data payload of bytes, whose data may look like instructions.
    public static func byteArrayPayload(_ bytes: [UInt8]) -> [UInt16] {
        var units: [UInt16] = [0x0300, 1, UInt16(bytes.count & 0xFFFF), UInt16(bytes.count >> 16)]
        var padded = bytes
        if padded.count % 2 == 1 { padded.append(0) }
        for index in stride(from: 0, to: padded.count, by: 2) {
            units.append(UInt16(padded[index]) | UInt16(padded[index + 1]) << 8)
        }
        return units
    }

    public static func make(strings: [String], methods: [[UInt16]]) -> Data {
        var data = Data(count: 0x70)
        func put32(_ value: Int, at offset: Int) {
            for byte in 0..<4 { data[offset + byte] = UInt8((value >> (8 * byte)) & 0xFF) }
        }
        func uleb(_ value: Int) -> [UInt8] {
            var value = value
            var out: [UInt8] = []
            repeat {
                var byte = UInt8(value & 0x7F)
                value >>= 7
                if value != 0 { byte |= 0x80 }
                out.append(byte)
            } while value != 0
            return out
        }
        func align4() { while data.count % 4 != 0 { data.append(0) } }

        data.replaceSubrange(0..<8, with: Array("dex\n035\0".utf8))
        put32(0x70, at: 0x24)
        // String IDs, then their data.
        let stringIDs = data.count
        put32(strings.count, at: 0x38)
        put32(stringIDs, at: 0x3C)
        data.append(Data(count: strings.count * 4))
        for (index, string) in strings.enumerated() {
            put32(data.count, at: stringIDs + index * 4)
            data.append(contentsOf: uleb(string.utf16.count) + Array(string.utf8) + [0])
        }
        // The code items.
        var codeOffsets: [Int] = []
        for units in methods {
            align4()
            codeOffsets.append(data.count)
            var item = Data(count: 16)
            item[0] = 4 // registers
            let size = units.count
            for byte in 0..<4 { item[12 + byte] = UInt8((size >> (8 * byte)) & 0xFF) }
            data.append(item)
            for unit in units { data.append(contentsOf: [UInt8(unit & 0xFF), UInt8(unit >> 8)]) }
        }
        // The class data: no fields, the methods as direct ones.
        let classData = data.count
        data.append(contentsOf: uleb(0) + uleb(0) + uleb(methods.count) + uleb(0))
        for (index, offset) in codeOffsets.enumerated() {
            data.append(contentsOf: uleb(index == 0 ? 0 : 1) + uleb(0x9) + uleb(offset))
        }
        // One class definition.
        align4()
        put32(1, at: 0x60)
        put32(data.count, at: 0x64)
        var classDef = Data(count: 32)
        for byte in 0..<4 { classDef[24 + byte] = UInt8((classData >> (8 * byte)) & 0xFF) }
        data.append(classDef)
        put32(data.count, at: 0x20)
        return data
    }
}
