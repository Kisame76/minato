import Foundation

/// The NEEDED names of a 64-bit little-endian shared object (ADR-0055, step a3): the libraries its dynamic section asks
/// the linker for. It reads a file the user brought, so every offset and size is checked against the file, and the
/// number and length of the names are capped; a file that does not hold together is refused, not read in part.
public enum ELFDynamic {
    /// More names than any library needs; a dynamic section with more is refused.
    public static let maxNeeded = 1024
    /// Longer than a library's file name can be.
    public static let maxNameLength = 255

    /// Control characters and the Unicode line and paragraph separators, which would break the line of a reason that
    /// names the library.
    private static func breaksALine(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .lineSeparator, .paragraphSeparator: true
        default: false
        }
    }

    /// The names of DT_NEEDED entries in order; none for a file without a PT_DYNAMIC, as a static program.
    public static func needed(in data: Data) throws -> [String] {
        let reader = ByteReader(data, context: "ELF file")
        // A slice of a larger Data counts from its own start; every index below is into the reader's copy of it.
        let bytes = reader.data
        guard reader.count >= 64, reader.data.prefix(4) == Data([0x7F, 0x45, 0x4C, 0x46]) else {
            throw APKError.invalidELF("missing ELF magic")
        }
        guard reader.data[4] == 2 else { throw APKError.invalidELF("not a 64-bit ELF") }
        guard reader.data[5] == 1 else { throw APKError.invalidELF("not little-endian") }
        let tableOffset = try reader.u64(at: 32)
        let entrySize = UInt64(try reader.u16(at: 54))
        let entryCount = UInt64(try reader.u16(at: 56))
        guard entrySize == 56, tableOffset <= UInt64(reader.count),
              tableOffset + entrySize * entryCount <= UInt64(reader.count) else {
            throw APKError.invalidELF("malformed program header table")
        }

        var loads: [(address: UInt64, offset: UInt64, size: UInt64)] = []
        var dynamic: (offset: UInt64, size: UInt64)?
        for index in 0..<entryCount {
            let entry = Int(tableOffset + index * entrySize)
            let type = try reader.u32(at: entry)
            let offset = try reader.u64(at: entry + 8)
            let address = try reader.u64(at: entry + 16)
            let fileSize = try reader.u64(at: entry + 32)
            if type == 1 {  // PT_LOAD
                loads.append((address, offset, fileSize))
            } else if type == 2, dynamic == nil {  // PT_DYNAMIC
                dynamic = (offset, fileSize)
            }
        }
        guard let dynamic else { return [] }
        guard dynamic.offset <= UInt64(reader.count), dynamic.size <= UInt64(reader.count) - dynamic.offset else {
            throw APKError.invalidELF("the dynamic section lies beyond the file")
        }

        var names: [UInt64] = []
        var strings: UInt64?
        var stringsSize: UInt64?
        var ended = false
        for index in 0..<(dynamic.size / 16) {
            let entry = Int(dynamic.offset + index * 16)
            let tag = try reader.u64(at: entry)
            let value = try reader.u64(at: entry + 8)
            if tag == 0 {  // DT_NULL
                ended = true
                break
            }
            switch tag {
            case 1:  // DT_NEEDED
                guard names.count < maxNeeded else { throw APKError.invalidELF("more than \(maxNeeded) NEEDED names") }
                names.append(value)
            case 5: strings = value  // DT_STRTAB
            case 10: stringsSize = value  // DT_STRSZ
            default: break
            }
        }
        guard ended else { throw APKError.invalidELF("the dynamic section has no end") }
        guard !names.isEmpty else { return [] }
        guard let strings, let stringsSize else { throw APKError.invalidELF("NEEDED names without a string table") }

        // The table's address lies in a PT_LOAD whose file part holds all of it.
        guard let load = loads.first(where: {
            strings >= $0.address && strings - $0.address < $0.size && stringsSize <= $0.size - (strings - $0.address)
        }) else { throw APKError.invalidELF("the string table lies outside the file part of the loaded segments") }
        // p_offset is the file's word: checked before it is added to anything.
        let inSegment = strings - load.address
        guard load.offset <= UInt64(reader.count), inSegment <= UInt64(reader.count) - load.offset else {
            throw APKError.invalidELF("the string table lies beyond the file")
        }
        let start = load.offset + inSegment
        guard stringsSize <= UInt64(reader.count) - start else {
            throw APKError.invalidELF("the string table lies beyond the file")
        }

        return try names.map { nameOffset in
            guard nameOffset < stringsSize else {
                throw APKError.invalidELF("a NEEDED name lies outside the string table")
            }
            let begin = bytes.startIndex + Int(start + nameOffset)
            let end = bytes.startIndex + Int(start + stringsSize)
            guard let terminator = bytes[begin..<end].firstIndex(of: 0), terminator - begin <= maxNameLength else {
                throw APKError.invalidELF("a NEEDED name has no end or is too long")
            }
            guard let name = String(data: bytes[begin..<terminator], encoding: .utf8),
                  !name.unicodeScalars.contains(where: Self.breaksALine) else {
                throw APKError.invalidELF("a NEEDED name is no text, or holds a control character")
            }
            return name
        }
    }
}
