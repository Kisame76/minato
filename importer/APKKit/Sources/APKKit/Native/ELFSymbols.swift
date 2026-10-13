import Foundation

/// The dynamic symbols of a 64-bit little-endian shared object (ADR-0055, step a4): what it defines and what it asks
/// the
/// linker for, with the version it asks for. It reads a file the user brought, from its section headers (the converter
/// refuses a file without them), so every offset and size is checked against the file and the counts are capped; a file
/// that does not hold together is refused, not read in part. The linker reads the same tables through the dynamic
/// section; a crafted file whose section headers disagree with it gets the report of its sections (a named limit).
public struct ELFSymbols: Sendable, Equatable {
    /// An undefined symbol of the library.
    public struct Reference: Sendable, Equatable {
        public var name: String
        /// The version it asks for, from the library's verneed; nil for none.
        public var version: String?
        /// A weak reference may stay unresolved.
        public var isWeak: Bool

        public init(name: String, version: String?, isWeak: Bool) {
            self.name = name
            self.version = version
            self.isWeak = isWeak
        }
    }

    /// The names of its global and weak definitions.
    public var defined: Set<String>
    /// Its undefined global and weak symbols, in table order.
    public var undefined: [Reference]

    /// More symbols than any library has; a table with more is refused.
    public static let maxSymbols = 1 << 21
    public static let maxNameLength = 4096

    public static func read(_ data: Data) throws -> ELFSymbols {
        let reader = ByteReader(data, context: "ELF file")
        let bytes = reader.data
        guard reader.count >= 64, bytes.prefix(4) == Data([0x7F, 0x45, 0x4C, 0x46]) else {
            throw APKError.invalidELF("missing ELF magic")
        }
        guard bytes[bytes.startIndex + 4] == 2 else { throw APKError.invalidELF("not a 64-bit ELF") }
        guard bytes[bytes.startIndex + 5] == 1 else { throw APKError.invalidELF("not little-endian") }
        let sectionTable = try reader.u64(at: 40)
        let entrySize = UInt64(try reader.u16(at: 58))
        let count = UInt64(try reader.u16(at: 60))
        guard count > 0, entrySize == 64, sectionTable <= UInt64(reader.count),
              count * entrySize <= UInt64(reader.count) - sectionTable else {
            throw APKError.invalidELF("no usable section headers")
        }

        struct Section {
            var type: UInt32
            var offset: UInt64
            var size: UInt64
            var link: UInt32
            var info: UInt32
        }
        var sections: [Section] = []
        for index in 0..<count {
            let at = Int(sectionTable + index * entrySize)
            sections.append(Section(
                type: try reader.u32(at: at + 4), offset: try reader.u64(at: at + 24),
                size: try reader.u64(at: at + 32),
                link: try reader.u32(at: at + 40), info: try reader.u32(at: at + 44)))
        }
        func contents(_ section: Section) throws -> Range<Int> {
            guard section.type != 8, section.offset <= UInt64(reader.count),
                  section.size <= UInt64(reader.count) - section.offset else {
                throw APKError.invalidELF("a section lies beyond the file")
            }
            return (bytes.startIndex + Int(section.offset))..<(bytes.startIndex + Int(section.offset + section.size))
        }

        guard let symtab = sections.first(where: { $0.type == 11 }) else {  // SHT_DYNSYM
            return ELFSymbols(defined: [], undefined: [])
        }
        guard Int(symtab.link) < sections.count, sections[Int(symtab.link)].type == 3 else {
            throw APKError.invalidELF("the symbol table has no string table")
        }
        let symbolRange = try contents(symtab)
        let stringRange = try contents(sections[Int(symtab.link)])
        guard symbolRange.count % 24 == 0, symbolRange.count / 24 <= maxSymbols else {
            throw APKError.invalidELF("the symbol table is malformed or has too many symbols")
        }
        // A name is made once for each string table offset, and all the names made together may hold only a few times
        // the table's size: symbols can share one long string at 2M different offsets, which would make GBs of names.
        var names: [UInt32: String] = [:]
        var budget = 8 * stringRange.count + (1 << 20)
        func string(at offset: UInt32) throws -> String {
            if let known = names[offset] { return known }
            guard Int(offset) < stringRange.count else {
                throw APKError.invalidELF("a name lies outside the string table")
            }
            let begin = stringRange.lowerBound + Int(offset)
            guard let end = bytes[begin..<stringRange.upperBound].firstIndex(of: 0), end - begin <= maxNameLength,
                  let name = String(data: bytes[begin..<end], encoding: .utf8),
                  !name.unicodeScalars.contains(where: { breaksALine($0) }) else {
                throw APKError.invalidELF("a name has no end, is too long, is no text or holds a control character")
            }
            budget -= name.utf8.count
            guard budget >= 0 else { throw APKError.invalidELF("the names add up to far more than the string table") }
            names[offset] = name
            return name
        }

        // The version a symbol asks for: .gnu.version's index through .gnu.version_r's auxiliary entries.
        var versionNames: [UInt16: String] = [:]
        if let verneed = sections.first(where: { $0.type == 0x6FFF_FFFE }) {
            let range = try contents(verneed)
            guard Int(verneed.link) < sections.count, sections[Int(verneed.link)].type == 3 else {
                throw APKError.invalidELF("the version table has no string table")
            }
            let names = try contents(sections[Int(verneed.link)])
            func versionString(_ offset: UInt32) throws -> String {
                guard Int(offset) < names.count else {
                    throw APKError.invalidELF("a version name lies outside its table")
                }
                let begin = names.lowerBound + Int(offset)
                guard let end = bytes[begin..<names.upperBound].firstIndex(of: 0), end - begin <= maxNameLength,
                      let text = String(data: bytes[begin..<end], encoding: .utf8) else {
                    throw APKError.invalidELF("a version name has no end or is no text")
                }
                return text
            }
            var at = 0
            var seen = 0
            for _ in 0..<verneed.info {
                guard at >= 0, at + 16 <= range.count else { throw APKError.invalidELF("the version table is cut off") }
                let base = range.lowerBound + at
                let auxCount = try reader.u16(at: base - bytes.startIndex + 2)
                var aux = at + Int(try reader.u32(at: base - bytes.startIndex + 8))
                for _ in 0..<auxCount {
                    seen += 1
                    guard seen <= 4096, aux >= 0, aux + 16 <= range.count else {
                        throw APKError.invalidELF("a version entry lies outside its table")
                    }
                    let entry = range.lowerBound + aux - bytes.startIndex
                    versionNames[try reader.u16(at: entry + 6)] = try versionString(try reader.u32(at: entry + 8))
                    let next = Int(try reader.u32(at: entry + 12))
                    if next == 0 { break }
                    aux += next
                }
                let next = Int(try reader.u32(at: base - bytes.startIndex + 12))
                if next == 0 { break }
                at += next
            }
        }
        var versionIndices: Range<Int>?
        if let versym = sections.first(where: { $0.type == 0x6FFF_FFFF }) {
            let range = try contents(versym)
            if range.count >= (symbolRange.count / 24) * 2 { versionIndices = range }
        }

        var defined: Set<String> = []
        var undefined: [Reference] = []
        for index in 1..<max(symbolRange.count / 24, 1) {
            let at = symbolRange.lowerBound + index * 24 - bytes.startIndex
            let info = bytes[bytes.startIndex + at + 4]
            let binding = info >> 4
            guard binding == 1 || binding == 2 else { continue }  // global or weak
            let nameOffset = try reader.u32(at: at)
            guard nameOffset != 0 else { continue }
            let name = try string(at: nameOffset)
            if try reader.u16(at: at + 6) != 0 {
                defined.insert(name)
            } else {
                var version: String?
                if let versionIndices {
                    let raw = try reader.u16(at: versionIndices.lowerBound - bytes.startIndex + index * 2) & 0x7FFF
                    if raw > 1 { version = versionNames[raw] }
                }
                undefined.append(Reference(name: name, version: version, isWeak: binding == 2))
            }
        }
        return ELFSymbols(defined: defined, undefined: undefined)
    }

    private static func breaksALine(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .lineSeparator, .paragraphSeparator: true
        default: false
        }
    }
}
