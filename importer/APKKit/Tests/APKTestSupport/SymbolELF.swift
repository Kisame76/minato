import APKPack
import Foundation

/// A small AArch64 shared object with a dynamic symbol table, as far as the reader of symbols needs: section headers
/// for .dynsym, .dynstr, .gnu.version and .gnu.version_r, a dynamic section with NEEDED names, and one PT_LOAD for the
/// file.
public enum SymbolELF {
    public struct Undefined {
        public var name: String
        /// The version the library asks for, from its verneed; nil for none.
        public var version: String?
        public var weak: Bool

        public init(_ name: String, version: String? = nil, weak: Bool = false) {
            self.name = name
            self.version = version
            self.weak = weak
        }
    }

    /// `rawStrings` go into .dynstr at offset 1, and `undefinedAt` are further undefined symbols whose names are the
    /// strings at those offsets, so that a file can point many symbols into one long string.
    public static func make(needed: [String] = [], defined: [String] = [], undefined: [Undefined] = [],
                            rawStrings: Data = Data(), undefinedAt: [UInt32] = []) -> Data {
        // .dynstr: names of NEEDED, symbols and version files/names.
        var strings = Data([0]) + rawStrings
        func add(_ text: String) -> UInt32 {
            let offset = UInt32(strings.count)
            strings += Data(text.utf8) + Data([0])
            return offset
        }
        let neededOffsets = needed.map(add)
        var versions: [String] = []
        for symbol in undefined {
            if let version = symbol.version, !versions.contains(version) { versions.append(version) }
        }
        let file = add("libfake.so")
        let versionNames = versions.map(add)
        // .dynsym: the null symbol, the defined ones, the undefined ones; and .gnu.version in step.
        var symbols = Data(count: 24)
        var versym: [UInt16] = [0]
        func symbol(name: UInt32, info: UInt8, section: UInt16) {
            var raw = ByteWriter()
            raw.u32(name); raw.u8(info); raw.u8(0); raw.u16(section); raw.u64(0); raw.u64(0)
            symbols += raw.data
        }
        for name in defined {
            symbol(name: add(name), info: 0x12, section: 1)  // GLOBAL FUNC, defined
            versym.append(1)
        }
        for item in undefined {
            symbol(name: add(item.name), info: item.weak ? 0x22 : 0x12, section: 0)
            versym.append(item.version.map { UInt16(2 + versions.firstIndex(of: $0)!) } ?? 1)
        }
        for offset in undefinedAt {
            symbol(name: offset, info: 0x12, section: 0)
            versym.append(1)
        }
        var versymData = ByteWriter()
        for value in versym { versymData.u16(value) }
        // .gnu.version_r: one Verneed for the fake file with an aux for each version, numbered from 2.
        var verneed = ByteWriter()
        if !versions.isEmpty {
            verneed.u16(1); verneed.u16(UInt16(versions.count)); verneed.u32(file); verneed.u32(16); verneed.u32(0)
            for (index, name) in versionNames.enumerated() {
                verneed.u32(0); verneed.u16(0); verneed.u16(UInt16(2 + index)); verneed.u32(name)
                verneed.u32(index + 1 == versionNames.count ? 0 : 16)
            }
        }
        var shstrings = Data([0])
        func shname(_ text: String) -> UInt32 {
            let offset = UInt32(shstrings.count)
            shstrings += Data(text.utf8) + Data([0])
            return offset
        }
        let names = [shname(".dynsym"), shname(".dynstr"), shname(".gnu.version"), shname(".gnu.version_r"),
                     shname(".shstrtab")]

        let headerSize = 64 + 2 * 56
        let entries = needed.count + 3
        let dynamicOffset = headerSize
        let stringsOffset = dynamicOffset + entries * 16
        let symbolsOffset = (stringsOffset + strings.count + 7) & ~7
        let versymOffset = symbolsOffset + symbols.count
        let verneedOffset = (versymOffset + versymData.data.count + 7) & ~7
        let shstrOffset = verneedOffset + verneed.data.count
        let sectionsOffset = (shstrOffset + shstrings.count + 7) & ~7
        let total = sectionsOffset + 6 * 64

        var out = ByteWriter()
        out.bytes([0x7F, 0x45, 0x4C, 0x46, 2, 1, 1, 0])
        out.zeros(8)
        out.u16(3); out.u16(183); out.u32(1)
        out.u64(0); out.u64(64); out.u64(UInt64(sectionsOffset))
        out.u32(0); out.u16(64); out.u16(56); out.u16(2); out.u16(64); out.u16(6); out.u16(5)
        out.u32(1); out.u32(5); out.u64(0); out.u64(0x1000); out.u64(0x1000); out.u64(UInt64(total))
        out.u64(UInt64(total)); out.u64(0x4000)
        out.u32(2); out.u32(6); out.u64(UInt64(dynamicOffset)); out.u64(0x1000 + UInt64(dynamicOffset))
        out.u64(0x1000 + UInt64(dynamicOffset))
        out.u64(UInt64(entries * 16)); out.u64(UInt64(entries * 16)); out.u64(8)
        for offset in neededOffsets { out.u64(1); out.u64(UInt64(offset)) }
        out.u64(5); out.u64(0x1000 + UInt64(stringsOffset))
        out.u64(10); out.u64(UInt64(strings.count))
        out.u64(0); out.u64(0)
        out.bytes(strings)
        out.zeros(symbolsOffset - stringsOffset - strings.count)
        out.bytes(symbols)
        out.bytes(versymData.data)
        out.zeros(verneedOffset - versymOffset - versymData.data.count)
        out.bytes(verneed.data)
        out.bytes(shstrings)
        out.zeros(sectionsOffset - shstrOffset - shstrings.count)
        func section(name: UInt32, type: UInt32, offset: Int, size: Int, link: UInt32, info: UInt32, entry: UInt64) {
            out.u32(name); out.u32(type); out.u64(2); out.u64(0); out.u64(UInt64(offset)); out.u64(UInt64(size))
            out.u32(link); out.u32(info); out.u64(8); out.u64(entry)
        }
        out.zeros(64)  // the null section
        section(name: names[0], type: 11, offset: symbolsOffset, size: symbols.count, link: 2, info: 1, entry: 24)
        section(name: names[1], type: 3, offset: stringsOffset, size: strings.count, link: 0, info: 0, entry: 0)
        section(name: names[2], type: 0x6FFF_FFFF, offset: versymOffset, size: versymData.data.count, link: 1, info: 0,
                entry: 2)
        section(name: names[3], type: 0x6FFF_FFFE, offset: verneedOffset, size: verneed.data.count, link: 2,
                info: versions.isEmpty ? 0 : 1, entry: 0)
        section(name: names[4], type: 3, offset: shstrOffset, size: shstrings.count, link: 0, info: 0, entry: 0)
        return out.data
    }
}
