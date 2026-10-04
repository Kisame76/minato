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
}
