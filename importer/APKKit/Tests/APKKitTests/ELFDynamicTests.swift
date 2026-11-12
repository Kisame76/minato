import APKTestSupport
import Foundation
import Testing

@testable import APKKit

/// The NEEDED names of a library (ADR-0055, step a3), read from user files: the dynamic section, its string table
/// through the address of a PT_LOAD, and nothing past the file.
@Suite("ELF NEEDED names")
struct ELFDynamicTests {
    @Test func readsTheNamesInOrder() throws {
        let file = DynamicELF.make(needed: ["libc.so", "liblog.so", "libhelper.so"])

        #expect(try ELFDynamic.needed(in: file) == ["libc.so", "liblog.so", "libhelper.so"])
    }

    @Test func aLibraryThatNeedsNothingHasNone() throws {
        #expect(try ELFDynamic.needed(in: DynamicELF.make(needed: [])) == [])
    }

    @Test func aFileWithoutADynamicSectionHasNone() throws {
        // A static program: program headers without PT_DYNAMIC.
        #expect(try ELFDynamic.needed(in: ELFBuilder(segments: [.load(align: 0x4000)]).build()) == [])
    }

    @Test func aFileThatIsNoELFIsRefused() {
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: Data("not an ELF file at all".utf8)) }
    }

    @Test(arguments: [10, 100, 170, 200])
    func aFileCutOffAnywhereIsRefusedNotRead(_ length: Int) {
        let file = DynamicELF.make(needed: ["libc.so", "libhelper.so"])
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file.prefix(length)) }
    }

    @Test func aNameWithoutItsEndIsRefused() {
        var file = DynamicELF.make(needed: ["libhelper.so"])
        file[file.count - 1] = 0x41  // the last byte of the string table is no NUL
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func aNameOffsetPastTheTableIsRefused() {
        var file = DynamicELF.make(needed: ["libhelper.so"])
        file[184] = 0xFF  // DT_NEEDED's value, low byte: far past DT_STRSZ
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func aDynamicSectionWithoutAnEndIsRefusedAtItsSize() {
        // DT_NULL turned into DT_NEEDED: the section ends with its size, not with a tag.
        var file = DynamicELF.make(needed: ["libhelper.so"])
        let nullTag = 176 + 3 * 16
        file[nullTag] = 1
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func manyNamesAreRefused() {
        let file = DynamicELF.make(needed: (0..<2000).map { "lib\($0).so" })
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    // MARK: - Hostile files

    @Test(arguments: [UInt64.max - 0x20, 0x1000_0000, UInt64(Int64.max)])
    func aLoadSegmentWhoseFileOffsetOverflowsOrLiesBeyondTheFileIsRefused(_ offset: UInt64) {
        var file = DynamicELF.make(needed: ["libhelper.so"])
        // p_offset of PT_LOAD
        file.replaceSubrange(72..<80, with: withUnsafeBytes(of: offset.littleEndian) { Data($0) })
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func aStringTableOutsideTheLoadedSegmentsIsRefused() {
        var file = DynamicELF.make(needed: ["libhelper.so"])
        let strtab = 176 + 1 * 16 + 8  // DT_STRTAB's value
        let outside = UInt64(0x90_0000)
        file.replaceSubrange(strtab..<(strtab + 8), with: withUnsafeBytes(of: outside.littleEndian) { Data($0) })
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func aNameLongerThanAFileNameIsRefused() {
        #expect(throws: APKError.self) {
            try ELFDynamic.needed(in: DynamicELF.make(needed: [String(repeating: "a", count: 300)]))
        }
    }

    @Test(arguments: [UInt8(0xFF), UInt8(0x0A), UInt8(0x07)])
    func aNameThatIsNoTextOrHoldsAControlCharacterIsRefused(_ byte: UInt8) {
        var file = DynamicELF.make(needed: ["libhelper.so"])
        file[file.count - 3] = byte  // inside the name, before its NUL
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test(arguments: ["lib\u{2028}x.so", "lib\u{2029}x.so", "lib\u{85}x.so", "lib\u{7F}x.so", "lib\u{9F}x.so"])
    func aNameWithAUnicodeLineBreakOrControlIsRefused(_ name: String) {
        // Joined with "\n" into a reason, such a name would forge a line, though it is not an ASCII control.
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: DynamicELF.make(needed: [name])) }
    }

    @Test func aProgramHeaderEntryOfAnotherSizeIsRefused() {
        // The converter refuses anything but 56 bytes too.
        var file = DynamicELF.make(needed: ["libhelper.so"])
        file[54] = 57
        #expect(throws: APKError.self) { try ELFDynamic.needed(in: file) }
    }

    @Test func aSliceOfALargerDataIsReadFromItsOwnStart() throws {
        let file = DynamicELF.make(needed: ["libc.so"])
        let slice = (Data(repeating: 0, count: 5) + file).dropFirst(5)

        #expect(try ELFDynamic.needed(in: slice) == ["libc.so"])
    }
}
