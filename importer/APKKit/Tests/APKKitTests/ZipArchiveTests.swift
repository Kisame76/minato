import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("ZipArchive")
struct ZipArchiveTests {
    @Test func listsEntriesInCentralDirectoryOrder() throws {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", "manifest")
        zip.add("classes.dex", "dex", method: .stored)
        zip.add("lib/arm64-v8a/libgame.so", "elf")
        let archive = try ZipArchive(data: zip.build().data)

        #expect(archive.entries.map(\.name) == ["AndroidManifest.xml", "classes.dex", "lib/arm64-v8a/libgame.so"])
    }

    @Test func readsStoredEntry() throws {
        var zip = ZipBuilder()
        zip.add("a.txt", "hello stored", method: .stored)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(try archive.data(named: "a.txt") == Data("hello stored".utf8))
    }

    @Test func readsDeflatedEntry() throws {
        let text = String(repeating: "deflate me ", count: 500)
        var zip = ZipBuilder()
        zip.add("b.txt", text, method: .deflated)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(try archive.data(named: "b.txt") == Data(text.utf8))
        #expect(archive.entry(named: "b.txt")?.method == .deflated)
    }

    @Test func readsEmptyEntries() throws {
        var zip = ZipBuilder()
        zip.add("empty-stored", Data(), method: .stored)
        zip.add("empty-deflated", Data(), method: .deflated)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(try archive.data(named: "empty-stored") == Data())
        #expect(try archive.data(named: "empty-deflated") == Data())
    }

    @Test func reportsDataOffsetOfAlignedStoredEntry() throws {
        var zip = ZipBuilder()
        zip.add("res/raw/x", "some filler so the next header is not aligned by chance")
        zip.add("lib/arm64-v8a/libmain.so", Data(repeating: 0x7F, count: 100), method: .stored, alignment: 16384)
        let built = zip.build()
        let archive = try ZipArchive(data: built.data)
        let entry = try #require(archive.entry(named: "lib/arm64-v8a/libmain.so"))

        #expect(try archive.dataOffset(of: entry) == UInt64(built.dataOffsets["lib/arm64-v8a/libmain.so"]!))
        #expect(try archive.dataOffset(of: entry) % 16384 == 0)
        #expect(try archive.data(for: entry) == Data(repeating: 0x7F, count: 100))
    }

    @Test func readsZip64Archive() throws {
        var zip = ZipBuilder()
        zip.forceZip64 = true
        zip.add("one", "first entry")
        zip.add("two", "second entry", method: .stored)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(archive.entries.map(\.name) == ["one", "two"])
        #expect(try archive.data(named: "one") == Data("first entry".utf8))
        #expect(try archive.data(named: "two") == Data("second entry".utf8))
    }

    @Test func findsEndRecordBehindArchiveComment() throws {
        var zip = ZipBuilder()
        zip.add("x", "y")
        zip.comment = Data("built by a tool that writes a comment".utf8)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(try archive.data(named: "x") == Data("y".utf8))
    }

    @Test func reportsCentralDirectoryOffset() throws {
        var zip = ZipBuilder()
        zip.add("x", "y")
        zip.beforeCentralDirectory = Data(repeating: 0xAB, count: 64)
        let built = zip.build()
        let archive = try ZipArchive(data: built.data)

        #expect(archive.centralDirectoryOffset == UInt64(built.centralDirectoryOffset))
    }

    @Test func rejectsDataThatIsNotAZipArchive() {
        #expect(throws: APKError.notAZipArchive) {
            _ = try ZipArchive(data: Data("definitely not a zip file".utf8))
        }
        #expect(throws: APKError.notAZipArchive) {
            _ = try ZipArchive(data: Data())
        }
    }

    @Test func detectsCorruptedEntryData() throws {
        var zip = ZipBuilder()
        zip.add("a.txt", "checksummed content", method: .stored)
        let built = zip.build()
        var bytes = built.data
        bytes[built.dataOffsets["a.txt"]!] ^= 0xFF
        let archive = try ZipArchive(data: bytes)

        #expect(throws: APKError.crcMismatch("a.txt")) {
            _ = try archive.data(named: "a.txt")
        }
    }

    @Test func refusesToInflateBeyondLimit() throws {
        var zip = ZipBuilder()
        zip.add("big", Data(count: 10_000))
        let archive = try ZipArchive(data: zip.build().data)

        #expect(throws: APKError.entryTooLarge("big", 10_000)) {
            _ = try archive.data(named: "big", maxSize: 1_000)
        }
    }

    @Test func throwsForMissingEntry() throws {
        var zip = ZipBuilder()
        zip.add("x", "y")
        let archive = try ZipArchive(data: zip.build().data)

        #expect(throws: APKError.entryNotFound("nope")) {
            _ = try archive.data(named: "nope")
        }
    }

    @Test func readsPrefixOfDeflatedEntry() throws {
        let content = Data((0..<50_000).map { UInt8(truncatingIfNeeded: $0 &* 31) })
        var zip = ZipBuilder()
        zip.add("lib/arm64-v8a/libbig.so", content, method: .deflated)
        zip.add("lib/arm64-v8a/libstored.so", content, method: .stored)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(try archive.prefix(of: archive.entry(named: "lib/arm64-v8a/libbig.so")!, count: 64) == content.prefix(64))
        #expect(try archive.prefix(of: archive.entry(named: "lib/arm64-v8a/libstored.so")!, count: 64) == content.prefix(64))
        #expect(try archive.prefix(of: archive.entry(named: "lib/arm64-v8a/libbig.so")!, count: 100_000) == content)
    }

    @Test func readsArchiveFromFile() throws {
        var zip = ZipBuilder()
        zip.add("file.txt", "from disk")
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("apkkit-\(UUID().uuidString).zip")
        try zip.build().data.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }

        let archive = try ZipArchive(url: url)
        #expect(try archive.data(named: "file.txt") == Data("from disk".utf8))
    }

    // MARK: - What libziparchive refuses (android-15.0.0_r1, ADR-0048 b1)

    /// `data` with each occurrence of `find` replaced by `replacement`, which is as long; `count` occurrences.
    static func replacing(_ find: [UInt8], with replacement: [UInt8], in data: Data, count: Int) throws -> Data {
        var data = data
        var found = 0
        var start = data.startIndex
        while let range = data.range(of: Data(find), in: start..<data.endIndex) {
            data.replaceSubrange(range, with: replacement)
            start = range.upperBound
            found += 1
        }
        try #require(found == count)
        return data
    }

    static func patch<T: FixedWidthInteger>(_ value: T, at offset: Int, in data: inout Data) {
        withUnsafeBytes(of: value.littleEndian) { data.replaceSubrange(offset..<offset + MemoryLayout<T>.size, with: $0) }
    }

    static func isInvalidName(_ error: any Error) -> Bool {
        if case .invalidEntryName = error as? APKError { true } else { false }
    }

    @Test func refusesTwoEntriesOfOneName() throws {
        // zip_cd_entry_map.cc:57-62 from zip_archive.cc:636-645: the Master Key class.
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", "first")
        zip.add("AndroidManifest.xml", "second")

        #expect(throws: APKError.duplicateEntry("AndroidManifest.xml")) { try ZipArchive(data: zip.build().data) }
    }

    @Test func namesAreComparedByTheirBytes() throws {
        // Swift's String equality makes the two one name; Android keeps both and finds each by its bytes.
        var zip = ZipBuilder()
        zip.add("\u{E9}.txt", "composed", method: .stored)
        zip.add("e\u{301}.txt", "decomposed", method: .stored)
        let archive = try ZipArchive(data: zip.build().data)

        #expect(archive.entries.count == 2)
        #expect(try archive.data(named: "\u{E9}.txt") == Data("composed".utf8))
        #expect(try archive.data(named: "e\u{301}.txt") == Data("decomposed".utf8))
    }

    @Test(arguments: [[0x00], [0x80], [0xFE, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80], [0xC3]] as [[UInt8]])
    func refusesANameThatIsNotUTF8OrHoldsANul(bytes: [UInt8]) throws {
        // entry_name_utils-inl.h:28-63 from zip_archive.cc:616-620; "@"s stand in for the bytes in both headers. 0xFE
        // is refused even with the continuation bytes its high bits would ask for.
        let standIn = "lib" + String(repeating: "@", count: bytes.count) + ".so"
        var zip = ZipBuilder()
        zip.add(standIn, "x", method: .stored)
        let data = try Self.replacing(Array(standIn.utf8), with: Array("lib".utf8) + bytes + Array(".so".utf8),
                                      in: zip.build().data, count: 2)

        #expect(performing: { try ZipArchive(data: data) }, throws: Self.isInvalidName)
        // A lead byte that ends the name.
        var end = ZipBuilder()
        end.add("lib.so@", "x", method: .stored)
        let cut = try Self.replacing(Array("lib.so@".utf8), with: Array("lib.so".utf8) + [0xC3], in: end.build().data, count: 2)
        #expect(performing: { try ZipArchive(data: cut) }, throws: Self.isInvalidName)
    }

    @Test func acceptsANameAndroidReadsThoughStrictUTF8DoesNot() throws {
        // Android checks the shape of each sequence only: an overlong "/" and a surrogate pass.
        var zip = ZipBuilder()
        zip.add("a@@.so", "x", method: .stored)
        zip.add("b@@@.so", "y", method: .stored)
        var data = try Self.replacing(Array("a@@.so".utf8), with: Array("a".utf8) + [0xC0, 0xAF] + Array(".so".utf8),
                                      in: zip.build().data, count: 2)
        data = try Self.replacing(Array("b@@@.so".utf8), with: Array("b".utf8) + [0xED, 0xA0, 0x80] + Array(".so".utf8),
                                  in: data, count: 2)

        #expect(try ZipArchive(data: data).entries.count == 2)
    }

    @Test func refusesALocalHeaderAtOrPastTheCentralDirectory() throws {
        // zip_archive.cc:610-614.
        var zip = ZipBuilder()
        zip.add("a.txt", "a", method: .stored)
        let output = zip.build()
        var data = output.data
        Self.patch(UInt32(output.centralDirectoryOffset), at: output.centralDirectoryOffset + 42, in: &data)

        #expect(throws: APKError.inconsistentEntry("a.txt", "its local header lies at or past the central directory")) {
            try ZipArchive(data: data)
        }
    }

    @Test func readsExactlyTheEntriesTheEndRecordCounts() throws {
        // zip_archive.cc:558-573: Android walks the records the end record counts, so one past them is no entry,
        // and a count past the records refuses the archive.
        var zip = ZipBuilder()
        zip.add("a.txt", "a", method: .stored)
        zip.add("b.txt", "b", method: .stored)
        let data = zip.build().data
        let endRecord = data.count - 22

        var fewer = data
        Self.patch(UInt16(1), at: endRecord + 8, in: &fewer)
        Self.patch(UInt16(1), at: endRecord + 10, in: &fewer)
        #expect(try ZipArchive(data: fewer).entries.map(\.name) == ["a.txt"])

        var more = data
        Self.patch(UInt16(3), at: endRecord + 8, in: &more)
        Self.patch(UInt16(3), at: endRecord + 10, in: &more)
        #expect(throws: APKError.truncated("central directory: 2 of 3 entries")) { try ZipArchive(data: more) }

        // zip_archive.cc:569-573: a counted record without its signature.
        let directory = zip.build().centralDirectoryOffset
        var unsigned = data
        let second = try #require(unsigned.range(of: Data([0x50, 0x4B, 0x01, 0x02]), in: (directory + 4)..<unsigned.count))
        unsigned[second.lowerBound] = 0
        #expect(throws: APKError.truncated("central directory: entry 1 has no signature")) { try ZipArchive(data: unsigned) }
    }

    @Test func refusesALocalHeaderNamingAnotherFile() throws {
        // zip_archive.cc:873-912, when the entry is read.
        var zip = ZipBuilder()
        zip.add("a.txt", "a", method: .stored)
        var otherName = zip.build().data
        otherName[30] = UInt8(ascii: "b")
        var otherLength = zip.build().data
        Self.patch(UInt16(4), at: 26, in: &otherLength)
        // A length that would also run into the central directory: the lengths differ first.
        var longLength = zip.build().data
        Self.patch(UInt16.max, at: 26, in: &longLength)

        for data in [otherName, otherLength, longLength] {
            let archive = try ZipArchive(data: data)
            #expect(throws: APKError.inconsistentEntry("a.txt", "its local header names another file")) {
                _ = try archive.data(named: "a.txt")
            }
        }
    }

    /// Entry "x", whose stored content ends in a local header for entry "y", so that y's name (`nameInside` false)
    /// or extra field runs into the central directory.
    static func headerAtTheEnd(nameInside: Bool) -> Data {
        var fake = ByteWriter()
        fake.u32(0x0403_4b50); fake.u16(20); fake.u16(0); fake.u16(0); fake.u16(0); fake.u16(0x21)
        fake.u32(0); fake.u32(0); fake.u32(0); fake.u16(1); fake.u16(nameInside ? 4 : 0)
        if nameInside { fake.bytes(Data("y".utf8)) }
        let content = fake.data
        var out = ByteWriter()
        out.u32(0x0403_4b50); out.u16(20); out.u16(0); out.u16(0); out.u16(0); out.u16(0x21)
        out.u32(CRC32.checksum(content)); out.u32(UInt32(content.count)); out.u32(UInt32(content.count))
        out.u16(1); out.u16(0); out.bytes(Data("x".utf8))
        let fakeOffset = out.count
        out.bytes(content)
        let directory = out.count
        for (name, offset, crc, size) in [("x", 0, CRC32.checksum(content), content.count), ("y", fakeOffset, 0, 0)] {
            out.u32(0x0201_4b50); out.u16(20); out.u16(20); out.u16(0); out.u16(0); out.u16(0); out.u16(0x21)
            out.u32(crc); out.u32(UInt32(size)); out.u32(UInt32(size))
            out.u16(1); out.u16(0); out.u16(0); out.u16(0); out.u16(0); out.u32(0); out.u32(UInt32(offset))
            out.bytes(Data(name.utf8))
        }
        let directorySize = out.count - directory
        out.u32(0x0605_4b50); out.u16(0); out.u16(0); out.u16(2); out.u16(2)
        out.u32(UInt32(directorySize)); out.u32(UInt32(directory)); out.u16(0)
        return out.data
    }

    @Test func refusesALocalHeaderThatRunsIntoTheCentralDirectory() throws {
        // zip_archive.cc:886-891 for the name, :913-919 for the extra field.
        for nameInside in [false, true] {
            let archive = try ZipArchive(data: Self.headerAtTheEnd(nameInside: nameInside))
            #expect(throws: APKError.inconsistentEntry("y", "its local header runs into the central directory")) {
                _ = try archive.data(named: "y")
            }
        }
        // zip_archive.cc:850-856: a local header that ends right at the central directory, with no name, extra field
        // or data after it (App's review).
        var empty = ZipBuilder()
        empty.add("", Data(), method: .stored)
        let archive = try ZipArchive(data: empty.build().data)
        #expect(throws: APKError.inconsistentEntry("", "its local header runs into the central directory")) {
            _ = try archive.data(named: "")
        }
    }

    @Test func refusesALocalHeaderWithOneZip64Size() throws {
        // zip_archive.cc:927-933: Android needs both sizes in ZIP64 or neither.
        var zip = ZipBuilder()
        zip.add("a.txt", "abc", method: .stored)
        var data = zip.build().data
        Self.patch(UInt32.max, at: 18, in: &data)
        let archive = try ZipArchive(data: data)

        #expect(throws: APKError.inconsistentEntry("a.txt", "its local header gives one size in ZIP64")) {
            _ = try archive.data(named: "a.txt")
        }
    }

    @Test func refusesLocalSizesOrCRCOtherThanTheCentralDirectorys() throws {
        // zip_archive.cc:986-996: without a data descriptor, the local header repeats them.
        var zip = ZipBuilder()
        zip.add("a.txt", "abc", method: .stored)
        for offset in [14, 18, 22] {
            var data = zip.build().data
            data[offset] ^= 0x01
            let archive = try ZipArchive(data: data)
            #expect(throws: APKError.inconsistentEntry("a.txt", "its local header's sizes or CRC-32 differ")) {
                _ = try archive.data(named: "a.txt")
            }
        }
    }

    @Test func refusesDataThatRunsIntoTheCentralDirectory() throws {
        // zip_archive.cc:1014-1033; the local header says the same, so only the range refuses.
        var zip = ZipBuilder()
        zip.add("a.txt", "abc", method: .stored)
        let output = zip.build()
        var data = output.data
        for offset in [18, 22, output.centralDirectoryOffset + 20, output.centralDirectoryOffset + 24] {
            Self.patch(UInt32(output.centralDirectoryOffset), at: offset, in: &data)
        }
        let archive = try ZipArchive(data: data)

        #expect(throws: APKError.inconsistentEntry("a.txt", "its data runs into the central directory")) {
            _ = try archive.data(named: "a.txt")
        }
        // A deflated entry's compressed size.
        var deflated = ZipBuilder()
        deflated.add("a.txt", String(repeating: "abc", count: 100), method: .deflated)
        let deflatedOutput = deflated.build()
        var compressed = deflatedOutput.data
        for offset in [18, deflatedOutput.centralDirectoryOffset + 20] {
            Self.patch(UInt32(deflatedOutput.centralDirectoryOffset), at: offset, in: &compressed)
        }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its data runs into the central directory")) {
            _ = try ZipArchive(data: compressed).data(named: "a.txt")
        }
        // A stored entry's uncompressed size counts too.
        var uncompressed = output.data
        for offset in [22, output.centralDirectoryOffset + 24] {
            Self.patch(UInt32(output.centralDirectoryOffset), at: offset, in: &uncompressed)
        }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its data runs into the central directory")) {
            _ = try ZipArchive(data: uncompressed).data(named: "a.txt")
        }
    }

    /// One entry, stored or deflated, whose local header leaves its CRC-32 and sizes to a data descriptor.
    static func describedArchive(signed: Bool = true, deflated: Bool = false, descriptorCRC: (UInt32) -> UInt32 = { $0 },
                                 compressedDelta: UInt32 = 0, uncompressedDelta: UInt32 = 0) -> (data: Data, content: Data) {
        let name = Data("described.txt".utf8), content = Data(String(repeating: "described content ", count: 20).utf8)
        let payload = deflated ? (try! (content as NSData).compressed(using: .zlib)) as Data : content
        let crc = CRC32.checksum(content), size = UInt32(content.count), packed = UInt32(payload.count)
        var out = ByteWriter()
        out.u32(0x0403_4b50); out.u16(20); out.u16(0x0808); out.u16(deflated ? 8 : 0); out.u16(0); out.u16(0x21)
        out.u32(0); out.u32(0); out.u32(0)
        out.u16(UInt16(name.count)); out.u16(0); out.bytes(name)
        out.bytes(payload)
        if signed { out.u32(0x0807_4b50) }
        out.u32(descriptorCRC(crc)); out.u32(packed &+ compressedDelta); out.u32(size &+ uncompressedDelta)
        let directory = out.count
        out.u32(0x0201_4b50); out.u16(20); out.u16(20); out.u16(0x0808); out.u16(deflated ? 8 : 0); out.u16(0); out.u16(0x21)
        out.u32(crc); out.u32(packed); out.u32(size)
        out.u16(UInt16(name.count)); out.u16(0); out.u16(0); out.u16(0); out.u16(0); out.u32(0); out.u32(0)
        out.bytes(name)
        let directorySize = out.count - directory
        out.u32(0x0605_4b50); out.u16(0); out.u16(0); out.u16(1); out.u16(1)
        out.u32(UInt32(directorySize)); out.u32(UInt32(directory)); out.u16(0)
        return (out.data, content)
    }

    @Test func checksTheDataDescriptorOfAnEntryReadWhole() throws {
        // zip_archive.cc:735-785, from 1618-1619 after an extraction; a prefix is no extraction.
        for signed in [true, false] {
            for deflated in [false, true] {
                let good = Self.describedArchive(signed: signed, deflated: deflated)
                #expect(try ZipArchive(data: good.data).data(named: "described.txt") == good.content)
            }
        }
        for sizes in [(1, 0), (0, 1)] as [(UInt32, UInt32)] {
            let wrong = Self.describedArchive(deflated: true, compressedDelta: sizes.0, uncompressedDelta: sizes.1)
            #expect(throws: APKError.inconsistentEntry("described.txt", "its data descriptor differs")) {
                _ = try ZipArchive(data: wrong.data).data(named: "described.txt")
            }
        }

        let bad = Self.describedArchive(descriptorCRC: { $0 ^ 1 })
        let archive = try ZipArchive(data: bad.data)
        let entry = try #require(archive.entry(named: "described.txt"))
        #expect(throws: APKError.inconsistentEntry("described.txt", "its data descriptor differs")) {
            _ = try archive.data(for: entry)
        }
        #expect(throws: APKError.inconsistentEntry("described.txt", "its data descriptor differs")) {
            try archive.forEachChunk(of: entry) { _ in }
        }
        #expect(try archive.prefix(of: entry, count: 4) == bad.content.prefix(4))
    }

    // MARK: - What libziparchive refuses, part two (ADR-0048 b1.1, the review)

    static func localHeader(_ name: [UInt8], _ data: Data, crc: UInt32? = nil) -> Data {
        var out = ByteWriter()
        let size = UInt32(data.count)
        out.u32(0x0403_4b50); out.u16(20); out.u16(0); out.u16(0); out.u16(0); out.u16(0x21)
        out.u32(crc ?? CRC32.checksum(data)); out.u32(size); out.u32(size); out.u16(UInt16(name.count)); out.u16(0)
        out.bytes(name); out.bytes(data)
        return out.data
    }

    static func directoryRecord(_ name: [UInt8], crc: UInt32, compressed: UInt32, uncompressed: UInt32, offset: UInt32,
                                extra: Data = Data()) -> Data {
        var out = ByteWriter()
        out.u32(0x0201_4b50); out.u16(20); out.u16(20); out.u16(0); out.u16(0); out.u16(0); out.u16(0x21)
        out.u32(crc); out.u32(compressed); out.u32(uncompressed)
        out.u16(UInt16(name.count)); out.u16(UInt16(extra.count)); out.u16(0); out.u16(0); out.u16(0); out.u32(0); out.u32(offset)
        out.bytes(name); out.bytes(extra)
        return out.data
    }

    static func endRecord(count: UInt16, size: Int, offset: Int, comment: Data = Data()) -> Data {
        var out = ByteWriter()
        out.u32(0x0605_4b50); out.u16(0); out.u16(0); out.u16(count); out.u16(count)
        out.u32(UInt32(size)); out.u32(UInt32(offset)); out.u16(UInt16(comment.count)); out.bytes(comment)
        return out.data
    }

    /// Stored entries with raw names: `prefix` before the first local header, `comment` and `trailing` after the
    /// end record.
    static func rawArchive(_ entries: [(name: [UInt8], data: Data)], prefix: Data = Data(), comment: Data = Data(),
                           trailing: Data = Data()) -> Data {
        var out = prefix, directory = Data()
        for (name, data) in entries {
            let crc = CRC32.checksum(data)
            directory += Self.directoryRecord(name, crc: crc, compressed: UInt32(data.count), uncompressed: UInt32(data.count),
                                              offset: UInt32(out.count))
            out += Self.localHeader(name, data)
        }
        let directoryOffset = out.count
        out += directory
        return out + Self.endRecord(count: UInt16(entries.count), size: directory.count, offset: directoryOffset, comment: comment)
            + trailing
    }

    static let good = Data("GOODGOOD".utf8), evil = Data("EVILEVIL".utf8)

    /// "a.txt" twice: a local header with `good` at 0 and one with `evil` after it. The central directory record
    /// points at the first with its 32-bit offset; its ZIP64 field `field` may point at the second.
    static func zip64Archive(compressed: UInt32 = 8, uncompressed: UInt32 = 8, offset: UInt32 = 0,
                             field: (_ second: Int) -> Data) -> Data {
        let name = Array("a.txt".utf8)
        var out = Self.localHeader(name, Self.good, crc: CRC32.checksum(Self.evil))
        let second = out.count
        out += Self.localHeader(name, Self.evil)
        let directory = Self.directoryRecord(name, crc: CRC32.checksum(Self.evil), compressed: compressed,
                                             uncompressed: uncompressed, offset: offset, extra: field(second))
        let directoryOffset = out.count
        return out + directory + Self.endRecord(count: 1, size: directory.count, offset: directoryOffset)
    }

    static func zip64Field(_ values: [UInt64], size: Int? = nil, trailing: Data = Data()) -> Data {
        var out = ByteWriter()
        out.u16(0x0001); out.u16(UInt16(size ?? values.count * 8))
        for value in values { out.u64(value) }
        return out.data + trailing
    }

    @Test func aCompleteZip64FieldGivesAllThreeValues() throws {
        // zip_archive.cc:492-511, read at :832-848: 24 bytes are all three values, whichever 32-bit field is
        // saturated, so Android reads the second header; a disk number after them (28 bytes) is ignored (:483-487).
        for disk in [false, true] {
            let data = Self.zip64Archive(compressed: UInt32.max, uncompressed: 99) { second in
                Self.zip64Field([8, 8, UInt64(second)], size: disk ? 28 : 24, trailing: disk ? Data(count: 4) : Data())
            }
            #expect(try ZipArchive(data: data).data(named: "a.txt") == Self.evil)
        }
        // The compressed size too, where only the size is saturated.
        let sizeOnly = Self.zip64Archive(compressed: 99, uncompressed: UInt32.max) { Self.zip64Field([8, 8, UInt64($0)]) }
        #expect(try ZipArchive(data: sizeOnly).data(named: "a.txt") == Self.evil)
    }

    @Test func aZip64FieldOfAnotherSizeThanItsValuesIsRefused() throws {
        // zip_archive.cc:513-529: the record is exactly what was read. An entry with only its size saturated fails
        // when it is read; one with a saturated offset fails the open (:598-608).
        let long = Self.zip64Archive(uncompressed: UInt32.max) { _ in Self.zip64Field([8, 0xDEAD]) }
        let archive = try ZipArchive(data: long)
        let entry = try #require(archive.entry(named: "a.txt"))
        #expect(throws: APKError.inconsistentEntry("a.txt", "its ZIP64 field does not hold its saturated values")) {
            _ = try archive.data(for: entry)
        }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its ZIP64 field does not hold its saturated values")) {
            _ = try archive.prefix(of: entry, count: 4)
        }
        let offset = Self.zip64Archive(offset: UInt32.max) { _ in Self.zip64Field([0, 0]) }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its ZIP64 field does not hold its saturated values")) {
            try ZipArchive(data: offset)
        }
        let missing = Self.zip64Archive(offset: UInt32.max) { _ in Data([0x99, 0x99, 8, 0]) + Data(count: 8) }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its ZIP64 field does not hold its saturated values")) {
            try ZipArchive(data: missing)
        }
    }

    @Test(arguments: [0x10000, UInt64(Int64.max) - 10, UInt64(1) << 63, UInt64.max - 5])
    func aCompleteZip64FieldsOffsetIsCheckedWhenTheEntryIsRead(offset: UInt64) throws {
        // zip_archive.cc:610-614 checks the 32-bit offset the open reads, :850-856 the ZIP64 one when the entry is read,
        // where an offset near the top overflows.
        let data = Self.zip64Archive(compressed: UInt32.max) { _ in Self.zip64Field([8, 8, offset]) }
        let archive = try ZipArchive(data: data)
        let entry = try #require(archive.entry(named: "a.txt"))
        let refusal = APKError.inconsistentEntry("a.txt", "its local header runs into the central directory")
        #expect(throws: refusal) { _ = try archive.data(for: entry) }
        #expect(throws: refusal) { _ = try archive.prefix(of: entry, count: 4) }
        #expect(throws: refusal) { try archive.forEachChunk(of: entry) { _ in } }
        #expect(throws: refusal) { _ = try archive.dataOffset(of: entry) }
    }

    @Test func aZip64OffsetFrom2To63PassesTheOpenAndFailsWhenRead() throws {
        // zip_archive.cc:596-611 holds the offset in an off64_t, where one from 2^63 is negative and passes the open;
        // reading the entry fails (ReadAtOffset, :1784).
        for offset in [UInt64(1) << 63, UInt64.max - 5] {
            let archive = try ZipArchive(data: Self.zip64Archive(offset: UInt32.max) { _ in Self.zip64Field([offset]) })
            let entry = try #require(archive.entry(named: "a.txt"))
            #expect(entry.localHeaderOffset == offset)
            let refusal = APKError.inconsistentEntry("a.txt", "its local header runs into the central directory")
            #expect(throws: refusal) { _ = try archive.data(for: entry) }
            #expect(throws: refusal) { _ = try archive.prefix(of: entry, count: 4) }
            #expect(throws: refusal) { try archive.forEachChunk(of: entry) { _ in } }
            #expect(throws: refusal) { _ = try archive.dataOffset(of: entry) }
        }
        // Below it, the open refuses an offset past the central directory.
        let below = Self.zip64Archive(offset: UInt32.max) { _ in Self.zip64Field([UInt64(Int64.max)]) }
        #expect(throws: APKError.inconsistentEntry("a.txt", "its local header lies at or past the central directory")) {
            try ZipArchive(data: below)
        }
    }

    @Test func aZip64HeaderRunningPastTheExtraFieldIsRefused() throws {
        // zip_archive.cc:459-462: every header's data lies within the extra field, checked before its ID counts and
        // before 28 bytes become 24 (:483-487), so 24 value bytes declared as 28 are refused.
        #expect(ZipArchive.zip64Values(Self.zip64Field([1, 2, 3], size: 28), uncompressed: .max, compressed: 0, offset: 0) == nil)
        let other = Data([0x99, 0x99, 0x40, 0]) + Self.zip64Field([1])
        #expect(ZipArchive.zip64Values(other, uncompressed: .max, compressed: 0, offset: 0) == nil)
        let refusal = APKError.inconsistentEntry("a.txt", "its ZIP64 field does not hold its saturated values")
        let offset = Self.zip64Archive(offset: UInt32.max) { Self.zip64Field([8, 8, UInt64($0)], size: 28) }
        #expect(throws: refusal) { try ZipArchive(data: offset) }
        let size = Self.zip64Archive(compressed: UInt32.max) { Self.zip64Field([8, 8, UInt64($0)], size: 28) }
        #expect(throws: refusal) { _ = try ZipArchive(data: size).data(named: "a.txt") }
    }

    @Test func refusesBytesAroundTheArchive() throws {
        // zip_archive.cc:318-324: nothing after the end record and its comment; :649-663: a local header at 0.
        let entries = [(name: Array("a.txt".utf8), data: Self.good)]
        #expect(try ZipArchive(data: Self.rawArchive(entries, comment: Data("note".utf8))).entries.count == 1)
        #expect(throws: APKError.invalidArchive("bytes follow its end record")) {
            try ZipArchive(data: Self.rawArchive(entries, trailing: Data("XYZ".utf8)))
        }
        #expect(throws: APKError.invalidArchive("it does not start with a local header")) {
            try ZipArchive(data: Self.rawArchive(entries, prefix: Data("JUNKJUNKJUNKJUNK".utf8)))
        }
    }

    @Test(arguments: [(8, 2), (10, 2), (12, 4), (16, 4)])
    func aSaturatedEndRecordNeedsZip64(field: Int, width: Int) throws {
        // zip_archive.cc:326-336, 214-275: a saturated field asks for a ZIP64 locator.
        var data = Self.rawArchive([(name: Array("a.txt".utf8), data: Self.good)])
        data.replaceSubrange((data.count - 22 + field)..<(data.count - 22 + field + width), with: Data(repeating: 0xFF, count: width))
        #expect(throws: APKError.invalidArchive("its end record asks for ZIP64, which is missing")) { try ZipArchive(data: data) }
    }

    @Test func aFullCommentOrNoRoomForALocatorAsksForZip64Too() throws {
        // zip_archive.cc:326-336 counts a comment of 0xFFFF bytes as saturated; :215-218 finds no room for a locator.
        #expect(throws: APKError.invalidArchive("its end record asks for ZIP64, which is missing")) {
            try ZipArchive(data: Self.rawArchive([(name: Array("a.txt".utf8), data: Self.good)], comment: Data(count: 0xFFFF)))
        }
        #expect(throws: APKError.invalidArchive("its end record asks for ZIP64, which is missing")) {
            try ZipArchive(data: Self.endRecord(count: 0xFFFF, size: 0, offset: 0))
        }
    }

    @Test func refusesACentralDirectoryPastItsEndRecord() throws {
        // zip_archive.cc:343-347 for the classic end record, :237-242 and :261-267 for the ZIP64 one.
        let entries = [(name: Array("a.txt".utf8), data: Self.good)]
        var data = Self.rawArchive(entries)
        Self.patch(UInt32(data.count - 22 - 45), at: data.count - 22 + 16, in: &data)
        #expect(throws: APKError.notAZipArchive) { try ZipArchive(data: data) }

        var zip = ZipBuilder()
        zip.forceZip64 = true
        zip.add("a.txt", "abc", method: .stored)
        let zip64 = zip.build().data
        #expect(try ZipArchive(data: zip64).entries.count == 1)
        let locator = try #require(zip64.range(of: Data([0x50, 0x4B, 0x06, 0x07]), options: .backwards))
        var record = zip64.subdata(in: 0..<zip64.count)
        let recordOffset = Int(record.subdata(in: (locator.lowerBound + 8)..<(locator.lowerBound + 16))
            .withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) })
        var unsigned = record
        unsigned[recordOffset] = 0
        #expect(throws: APKError.invalidArchive("its end record asks for ZIP64, which is missing")) { try ZipArchive(data: unsigned) }
        var overlapping = record
        Self.patch(UInt64(locator.lowerBound - 10), at: locator.lowerBound + 8, in: &overlapping)
        #expect(throws: APKError.notAZipArchive) { try ZipArchive(data: overlapping) }
        Self.patch(UInt64(recordOffset), at: recordOffset + 48, in: &record)
        #expect(throws: APKError.notAZipArchive) { try ZipArchive(data: record) }
    }

    @Test func aZip64LocatorCountsOnlyForASaturatedEndRecord() throws {
        // zip_archive.cc:326-347: an end record with no saturated field is read as it is, a ZIP64 locator before it
        // or not. Here the ZIP64 record points at another central directory.
        let name = Array("a.txt".utf8)
        var out = Self.localHeader(name, Self.good)
        let second = out.count
        out += Self.localHeader(name, Self.evil)
        let classic = Self.directoryRecord(name, crc: CRC32.checksum(Self.evil), compressed: 8, uncompressed: 8,
                                           offset: UInt32(second))
        let classicOffset = out.count
        out += classic
        let other = Self.directoryRecord(name, crc: CRC32.checksum(Self.good), compressed: 8, uncompressed: 8, offset: 0)
        let otherOffset = out.count
        out += other
        var zip64 = ByteWriter()
        let recordOffset = out.count
        zip64.u32(0x0606_4b50); zip64.u64(44); zip64.u16(45); zip64.u16(45); zip64.u32(0); zip64.u32(0)
        zip64.u64(1); zip64.u64(1); zip64.u64(UInt64(other.count)); zip64.u64(UInt64(otherOffset))
        zip64.u32(0x0706_4b50); zip64.u32(0); zip64.u64(UInt64(recordOffset)); zip64.u32(1)
        out += zip64.data + Self.endRecord(count: 1, size: classic.count, offset: classicOffset)

        #expect(try ZipArchive(data: out).data(named: "a.txt") == Self.evil)
    }

    @Test func refusesMoreEntriesThanItsCapBeforeReadingThem() throws {
        // ADR-0049 a1: the end record's count is checked before the central directory is read, so an archive that
        // counts more entries than the cap is refused by its count alone, not by the walk that would find them missing.
        let data = Self.rawArchive([(Array("a.txt".utf8), Data("a".utf8)), (Array("b.txt".utf8), Data("b".utf8))])
        #expect(try ZipArchive(data: data, maxEntries: 2).entries.count == 2)
        #expect(throws: APKError.invalidArchive("it holds 2 entries, more than the 1 Minato reads")) {
            try ZipArchive(data: data, maxEntries: 1)
        }
        var more = data
        Self.patch(UInt16(60_000), at: data.count - 22 + 8, in: &more)
        Self.patch(UInt16(60_000), at: data.count - 22 + 10, in: &more)
        #expect(throws: APKError.invalidArchive("it holds 60000 entries, more than the 4096 Minato reads")) {
            try ZipArchive(data: more, maxEntries: 4096)
        }
    }

    @Test func refusesACentralDirectoryOverItsByteCapBeforeReadingIt() throws {
        // The entry cap bounds the records, not their bytes: names, extra fields and comments of 64 KiB each would
        // still fill 512 MiB. The end record's size is checked against its own cap before the directory is read.
        let data = Self.rawArchive([(Array("a.txt".utf8), Data("a".utf8)), (Array("b.txt".utf8), Data("b".utf8))])
        let size = (try ZipArchive(data: data)).centralDirectorySize
        #expect(try ZipArchive(data: data, maxDirectoryBytes: size).entries.count == 2)
        #expect(throws: APKError.invalidArchive("its central directory is \(size) bytes, more than the \(size - 1) Minato reads")) {
            try ZipArchive(data: data, maxDirectoryBytes: size - 1)
        }
        #expect(ZipArchive.defaultMaxDirectoryBytes == 128 << 20)
    }

    @Test func theDefaultCapRefusesAZip64CountOfMillions() throws {
        // libziparchive has no cap but ZIP64's (zip_archive.cc:554-573); ours bounds the entries' memory.
        let name = Array("a.txt".utf8)
        var out = Self.localHeader(name, Self.good)
        let record = Self.directoryRecord(name, crc: CRC32.checksum(Self.good), compressed: 8, uncompressed: 8, offset: 0)
        let directoryOffset = out.count
        out += record
        var zip64 = ByteWriter()
        let recordOffset = out.count
        let count = ZipArchive.defaultMaxEntries + 1
        zip64.u32(0x0606_4b50); zip64.u64(44); zip64.u16(45); zip64.u16(45); zip64.u32(0); zip64.u32(0)
        zip64.u64(count); zip64.u64(count); zip64.u64(UInt64(record.count)); zip64.u64(UInt64(directoryOffset))
        zip64.u32(0x0706_4b50); zip64.u32(0); zip64.u64(UInt64(recordOffset)); zip64.u32(1)
        out += zip64.data + Self.endRecord(count: .max, size: record.count, offset: directoryOffset)

        #expect(ZipArchive.defaultMaxEntries == 1 << 20)
        #expect(throws: APKError.invalidArchive("it holds \(count) entries, more than the \(count - 1) Minato reads")) {
            try ZipArchive(data: out)
        }
    }

    @Test func aZip64DirectorySizeOverTheByteCapIsRefusedToo() throws {
        // The ZIP64 end record's size is the one checked when the classic record is saturated.
        let name = Array("a.txt".utf8)
        var out = Self.localHeader(name, Self.good)
        let record = Self.directoryRecord(name, crc: CRC32.checksum(Self.good), compressed: 8, uncompressed: 8, offset: 0)
        let directoryOffset = out.count
        out += record
        var zip64 = ByteWriter()
        let recordOffset = out.count
        zip64.u32(0x0606_4b50); zip64.u64(44); zip64.u16(45); zip64.u16(45); zip64.u32(0); zip64.u32(0)
        zip64.u64(1); zip64.u64(1); zip64.u64(UInt64(record.count)); zip64.u64(UInt64(directoryOffset))
        zip64.u32(0x0706_4b50); zip64.u32(0); zip64.u64(UInt64(recordOffset)); zip64.u32(1)
        out += zip64.data + Self.endRecord(count: .max, size: record.count, offset: directoryOffset)

        #expect(try ZipArchive(data: out, maxDirectoryBytes: UInt64(record.count)).entries.count == 1)
        #expect(throws: APKError.invalidArchive(
            "its central directory is \(record.count) bytes, more than the \(record.count - 1) Minato reads")) {
            try ZipArchive(data: out, maxDirectoryBytes: UInt64(record.count - 1))
        }
    }

    @Test func anEmptyArchiveIsRefused() throws {
        // zip_archive.cc:409-414: kEmptyArchive.
        #expect(throws: APKError.invalidArchive("it holds no entries")) { try ZipArchive(data: Self.rawArchive([])) }
    }

    @Test func aNameThatIsNotUTF8IsNeverFoundForAnother() throws {
        // A raw name Android accepts but UTF-8 does not (an overlong form) must not read as another entry's name, and
        // a byte-order mark is part of a name, which Foundation's decoding drops (the review).
        let entries: [(name: [UInt8], data: Data)] = [
            ([0x61, 0xC0, 0xAF], Data("ONE".utf8)), ([0x61, 0xC3, 0x80, 0xC2, 0xAF], Data("TWO".utf8)),
            ([0xEF, 0xBB, 0xBF] + Array("b.txt".utf8), Data("BOM".utf8)), (Array("b.txt".utf8), Data("PLAIN".utf8)),
            ([0xEF, 0xBB, 0xBF] + Array("lib/arm64-v8a/libx.so".utf8), Data("LIB".utf8)),
            // What a lossy decoding of the first would read as: replacement characters, which a name may hold.
            ([0x61, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD], Data("REPLACED".utf8)),
        ]
        let archive = try ZipArchive(data: Self.rawArchive(entries))

        #expect(Set(archive.entries.map(\.name)).count == entries.count)
        for entry in archive.entries {
            let found = archive.entry(named: entry.name)
            #expect(found == nil || found == entry)
        }
        #expect(archive.entry(named: archive.entries[0].name) == nil)
        #expect(try archive.data(named: "\u{FEFF}b.txt") == Data("BOM".utf8))
        #expect(!archive.entries.contains { $0.name.hasPrefix("lib/") })
    }
}
