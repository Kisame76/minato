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
}
