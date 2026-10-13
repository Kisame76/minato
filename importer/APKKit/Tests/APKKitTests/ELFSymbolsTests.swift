import APKTestSupport
import Foundation
import Testing

@testable import APKKit

/// The dynamic symbols of a library (ADR-0055, step a4), read from a user's file: what it defines, and what it asks
/// for with the version it asks for, from its section headers; every offset and size checked against the file.
@Suite("ELF dynamic symbols")
struct ELFSymbolsTests {
    @Test func readsWhatALibraryDefinesAndWhatItAsksFor() throws {
        let file = SymbolELF.make(
            defined: ["game_init", "game_run"],
            undefined: [.init("malloc"), .init("AAsset_close", version: "LIBANDROID"), .init("optional", weak: true)])

        let symbols = try ELFSymbols.read(file)

        #expect(symbols.defined == ["game_init", "game_run"])
        #expect(symbols.undefined == [
            ELFSymbols.Reference(name: "malloc", version: nil, isWeak: false),
            ELFSymbols.Reference(name: "AAsset_close", version: "LIBANDROID", isWeak: false),
            ELFSymbols.Reference(name: "optional", version: nil, isWeak: true),
        ])
    }

    @Test func aLibraryWithoutSymbolsHasNone() throws {
        let symbols = try ELFSymbols.read(SymbolELF.make())

        #expect(symbols.defined.isEmpty && symbols.undefined.isEmpty)
    }

    @Test func aFileWithoutSectionHeadersHasNoVerdict() {
        // The converter refuses such a file; the reader does not guess at its symbols.
        #expect(throws: APKError.self) { try ELFSymbols.read(DynamicELF.make(needed: ["libc.so"])) }
    }

    @Test func aFileThatIsNoELFIsRefused() {
        #expect(throws: APKError.self) { try ELFSymbols.read(Data("not an ELF file at all, no".utf8)) }
    }

    @Test(arguments: [100, 200, 260, 300, 400])
    func aFileCutOffAnywhereIsRefusedNotRead(_ length: Int) {
        let file = SymbolELF.make(defined: ["a"], undefined: [.init("b", version: "V")])
        #expect(throws: APKError.self) { try ELFSymbols.read(file.prefix(length)) }
    }

    @Test func aSectionTableBeyondTheFileIsRefused() {
        var file = SymbolELF.make(defined: ["a"])
        file.replaceSubrange(40..<48, with: withUnsafeBytes(of: UInt64.max.littleEndian) { Data($0) })  // e_shoff
        #expect(throws: APKError.self) { try ELFSymbols.read(file) }
    }

    @Test func aNameOffsetPastTheStringTableIsRefused() {
        var file = SymbolELF.make(undefined: [.init("b")])
        // The first real symbol's st_name: the table starts after the null symbol.
        let symtab = ELFSymbolsTests.sectionOffset(of: 11, in: file)
        file.replaceSubrange((symtab + 24)..<(symtab + 28), with: Data([0xFF, 0xFF, 0xFF, 0x7F]))
        #expect(throws: APKError.self) { try ELFSymbols.read(file) }
    }

    @Test func aSymbolNameWithAControlCharacterIsRefused() {
        var file = SymbolELF.make(undefined: [.init("bad_name")])
        let index = file.range(of: Data("bad_name".utf8))!.lowerBound + 3
        file[index] = 0x0A
        #expect(throws: APKError.self) { try ELFSymbols.read(file) }
    }

    @Test func aVersionIndexWithoutAVerneedIsNoVersion() throws {
        var file = SymbolELF.make(undefined: [.init("b", version: "V")])
        let versym = ELFSymbolsTests.sectionOffset(of: 0x6FFF_FFFF, in: file)
        file[versym + 2] = 0x63  // version 99, which no verneed names
        let symbols = try ELFSymbols.read(file)

        #expect(symbols.undefined.map(\.version) == [nil])
    }

    @Test func aSliceOfALargerDataIsReadFromItsOwnStart() throws {
        let file = SymbolELF.make(defined: ["a"])
        let slice = (Data(repeating: 0, count: 7) + file).dropFirst(7)

        #expect(try ELFSymbols.read(slice).defined == ["a"])
    }

    /// File offset of the first section of `type`, read from the section header table.
    static func sectionOffset(of type: UInt32, in file: Data) -> Int {
        func u64(_ at: Int) -> Int { Int(file.loadLittleEndian(at, as: UInt64.self)) }
        func u32(_ at: Int) -> UInt32 { file.loadLittleEndian(at, as: UInt32.self) }
        let table = u64(40)
        for index in 0..<6 where u32(table + index * 64 + 4) == type { return u64(table + index * 64 + 24) }
        fatalError("no section of type \(type)")
    }
}

private extension Data {
    func loadLittleEndian<T: FixedWidthInteger>(_ offset: Int, as type: T.Type) -> T {
        var value: T = 0
        for index in 0..<MemoryLayout<T>.size { value |= T(self[startIndex + offset + index]) << (8 * index) }
        return value
    }

    @Test func manySymbolsPointingAtOneLongNameCostOneName() throws {
        // 400k undefined symbols at one 4000 byte name: a string for each of them took 2 GB and 28 s (the lead's review).
        let name = Data(repeating: 0x61, count: 4000) + Data([0])
        let file = SymbolELF.make(rawStrings: name, undefinedAt: Array(repeating: 1, count: 400_000))
        let start = ContinuousClock.now

        let symbols = try ELFSymbols.read(file)

        #expect(symbols.undefined.count == 400_000)
        #expect(ContinuousClock.now - start < .seconds(10))
    }

    @Test func namesInTotalLongerThanTheStringTableAllowsAreRefused() {
        // 200k symbols at distinct offsets into runs of 4000 letters: 2000 bytes of name each on average, 400 MB
        // from a table of 1 MB.
        let runs = 250
        let run = Data(repeating: 0x61, count: 4000) + Data([0])
        var table = Data()
        for _ in 0..<runs { table += run }
        var offsets: [UInt32] = []
        for index in 0..<200_000 { offsets.append(UInt32(1 + (index / 800) * 4001 + index % 800)) }
        let file = SymbolELF.make(rawStrings: table, undefinedAt: offsets)

        #expect(throws: APKError.self) { try ELFSymbols.read(file) }
    }
}
