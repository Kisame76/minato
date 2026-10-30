import Foundation

/// The string constants a dex file's code loads (`const-string` and `const-string/jumbo`), read from its header, string
/// IDs, class definitions and code items as dex_file.h and dex_instruction_list.h lay them out. The import names the
/// app libraries that Java loads by them (ADR-0055, decision 2). A file it cannot read gives what it read up to there.
public enum DexStrings {
    public static func constants(in dex: Data) -> Set<String> {
        let bytes = [UInt8](dex)
        guard bytes.count >= 0x70, bytes.starts(with: Array("dex\n".utf8)) else { return [] }
        let reader = Reader(bytes: bytes)
        guard let stringCount = reader.u32(0x38), let stringOffset = reader.u32(0x3C),
              let classCount = reader.u32(0x60), let classOffset = reader.u32(0x64) else { return [] }
        var indices: Set<Int> = []
        for index in 0..<Int(classCount) {
            guard let dataOffset = reader.u32(Int(classOffset) + index * 32 + 24) else { break }
            if dataOffset != 0 { reader.scanClassData(at: Int(dataOffset), into: &indices) }
        }
        return Set(indices.compactMap { index in
            guard index < stringCount, let dataOffset = reader.u32(Int(stringOffset) + index * 4) else { return nil }
            return reader.string(at: Int(dataOffset))
        })
    }

    private struct Reader {
        let bytes: [UInt8]

        func u16(_ offset: Int) -> UInt16? {
            guard offset >= 0, offset + 2 <= bytes.count else { return nil }
            return UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
        }

        func u32(_ offset: Int) -> UInt32? {
            guard let low = u16(offset), let high = u16(offset + 2) else { return nil }
            return UInt32(low) | UInt32(high) << 16
        }

        func uleb128(_ offset: inout Int) -> Int? {
            var result = 0
            for shift in stride(from: 0, through: 28, by: 7) {
                guard offset < bytes.count else { return nil }
                let byte = bytes[offset]
                offset += 1
                result |= Int(byte & 0x7F) << shift
                if byte & 0x80 == 0 { return result }
            }
            return nil
        }

        /// A string_data_item: its UTF-16 length, then MUTF-8 up to the NUL. Only ASCII names matter here.
        func string(at offset: Int) -> String? {
            var position = offset
            guard uleb128(&position) != nil, let end = bytes[position...].firstIndex(of: 0) else { return nil }
            return String(decoding: bytes[position..<end], as: UTF8.self)
        }

        /// A class_data_item: the field lists are skipped, each method's code item is scanned.
        func scanClassData(at offset: Int, into indices: inout Set<Int>) {
            var position = offset
            guard let staticFields = uleb128(&position), let instanceFields = uleb128(&position),
                  let directMethods = uleb128(&position), let virtualMethods = uleb128(&position) else { return }
            for _ in 0..<(staticFields + instanceFields) {
                guard uleb128(&position) != nil, uleb128(&position) != nil else { return }
            }
            for _ in 0..<(directMethods + virtualMethods) {
                guard uleb128(&position) != nil, uleb128(&position) != nil, let code = uleb128(&position) else {
                    return
                }
                if code != 0 { scanCode(at: code, into: &indices) }
            }
        }

        /// A code_item's instructions, one after another by their formats' widths.
        func scanCode(at offset: Int, into indices: inout Set<Int>) {
            guard let units = u32(offset + 12) else { return }
            let start = offset + 16
            let end = start + Int(units) * 2
            guard end <= bytes.count else { return }
            var position = start
            while position < end, let unit = u16(position) {
                let opcode = Int(unit & 0xFF)
                if opcode == 0x1A, let index = u16(position + 2) {
                    indices.insert(Int(index))
                } else if opcode == 0x1B, let index = u32(position + 2) {
                    indices.insert(Int(index))
                }
                guard let width = width(of: unit, at: position), width > 0 else { return }
                position += width * 2
            }
        }

        /// An instruction's length in code units: its format's, or a payload's after a nop's identifying unit.
        func width(of unit: UInt16, at offset: Int) -> Int? {
            switch unit {
            case 0x0100: // packed-switch-payload: ident, size, first key, size targets of 2 units each
                return u16(offset + 2).map { Int($0) * 2 + 4 }
            case 0x0200: // sparse-switch-payload: ident, size, size keys and size targets of 2 units each
                return u16(offset + 2).map { Int($0) * 4 + 2 }
            case 0x0300: // fill-array-data-payload: ident, element width, size (2 units), the data padded to units
                guard let elementWidth = u16(offset + 2), let size = u32(offset + 4) else { return nil }
                return (Int(elementWidth) * Int(size) + 1) / 2 + 4
            default:
                return Self.widths[Int(unit & 0xFF)]
            }
        }

        static let widths: [Int] = {
            var table = [Int](repeating: 1, count: 256)
            func set(_ range: ClosedRange<Int>, _ width: Int) { for opcode in range { table[opcode] = width } }
            set(0x02...0x02, 2); set(0x03...0x03, 3); set(0x05...0x05, 2); set(0x06...0x06, 3)
            set(0x08...0x08, 2); set(0x09...0x09, 3)
            set(0x13...0x13, 2); set(0x14...0x14, 3); set(0x15...0x16, 2); set(0x17...0x17, 3)
            set(0x18...0x18, 5); set(0x19...0x1A, 2); set(0x1B...0x1B, 3); set(0x1C...0x1C, 2)
            set(0x1F...0x20, 2); set(0x22...0x23, 2); set(0x24...0x26, 3); set(0x29...0x29, 2)
            set(0x2A...0x2C, 3); set(0x2D...0x3D, 2); set(0x44...0x6D, 2); set(0x6E...0x72, 3)
            set(0x74...0x78, 3); set(0x90...0xAF, 2); set(0xD0...0xE2, 2); set(0xFA...0xFB, 4)
            set(0xFC...0xFD, 3); set(0xFE...0xFF, 2)
            return table
        }()
    }
}
