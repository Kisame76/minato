import Darwin
import Foundation
import Testing

@testable import APKSession

/// ADR-0057 step e: the host takes a file the app finished in MediaStore by a descriptor walked from its own record of
/// the data folder, and copies only from that descriptor into a folder of its own, for Photos.
@Suite("MediaExports")
final class MediaExportsTests {
    let root: URL
    let data: URL
    let copies: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "media-exports-\(UUID().uuidString)")
        data = root.appending(path: "data")
        copies = root.appending(path: "copies")
        try FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: copies, withIntermediateDirectories: true)
    }

    deinit { try? FileManager.default.removeItem(at: root) }

    func put(_ path: String, _ text: String) throws {
        let url = data.appending(path: path)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }

    func exports(countLimit: Int = MediaExports.countLimit, fileLimit: Int64 = MediaExports.fileLimit,
                 totalLimit: Int64 = MediaExports.totalLimit) -> MediaExports {
        MediaExports(dataFolder: data, countLimit: countLimit, fileLimit: fileLimit, totalLimit: totalLimit)
    }

    @Test func anImageIsTakenAndCopiedUnderItsName() throws {
        try put("files/Pictures/Hello world.png", "png bytes")
        let job = try #require(exports().take("1\tfiles/Pictures/Hello world.png"))
        #expect(job.kind == .image)
        #expect(job.size == 9)
        let copy = try #require(job.copy(into: copies))
        #expect(copy.lastPathComponent == "Hello world.png")
        #expect(copy.path.hasPrefix(copies.path + "/"))
        #expect(try String(contentsOf: copy, encoding: .utf8) == "png bytes")
        #expect(try String(contentsOf: data.appending(path: "files/Pictures/Hello world.png"), encoding: .utf8)
            == "png bytes")  // untouched
    }

    @Test func aVideoIsKindTwo() throws {
        try put("v.mp4", "mp4")
        #expect(try #require(exports().take("2\tv.mp4")).kind == .video)
    }

    @Test func twoCopiesOfOneNameDoNotMeet() throws {
        try put("a.png", "first")
        let exports = exports()
        let one = try #require(exports.take("1\ta.png")?.copy(into: copies))
        try put("a.png", "second")
        let two = try #require(exports.take("1\ta.png")?.copy(into: copies))
        #expect(one != two)
        #expect(try String(contentsOf: one, encoding: .utf8) == "first")
        #expect(try String(contentsOf: two, encoding: .utf8) == "second")
    }

    @Test func aTextOutsideTheContractIsRefused() throws {
        try put("a.png", "a")
        for text in ["", "1", "1\t", "3\ta.png", "0\ta.png", "1 a.png", "1\t/a.png", "1\ta.png/", "1\t./a.png",
                     "1\t../data/a.png", "1\tx//a.png", "12\ta.png"] {
            #expect(exports().take(text) == nil, "\(text)")
        }
        let deep = Array(repeating: "d", count: 16).joined(separator: "/")
        try put(deep + "/a.png", "a")
        #expect(exports().take("1\t" + deep + "/a.png") == nil)  // 17 names
    }

    @Test func aLinkOnTheWayOrAsTheFileIsRefused() throws {
        let elsewhere = root.appending(path: "elsewhere")
        try FileManager.default.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        try Data("secret".utf8).write(to: elsewhere.appending(path: "a.png"))
        try FileManager.default.createSymbolicLink(at: data.appending(path: "folder"), withDestinationURL: elsewhere)
        #expect(exports().take("1\tfolder/a.png") == nil)
        try FileManager.default.createSymbolicLink(at: data.appending(path: "b.png"),
                                                   withDestinationURL: elsewhere.appending(path: "a.png"))
        #expect(exports().take("1\tb.png") == nil)
    }

    @Test func aFolderAFIFOOrAHardLinkIsRefused() throws {
        try FileManager.default.createDirectory(at: data.appending(path: "folder.png"),
                                                withIntermediateDirectories: true)
        #expect(exports().take("1\tfolder.png") == nil)
        #expect(mkfifo(data.appending(path: "pipe.png").path, 0o600) == 0)
        #expect(exports().take("1\tpipe.png") == nil)  // returns, as the open does not block
        try put("one.png", "one")
        #expect(link(data.appending(path: "one.png").path, data.appending(path: "two.png").path) == 0)
        #expect(exports().take("1\ttwo.png") == nil)
    }

    @Test func aMissingFileIsRefused() {
        #expect(exports().take("1\tmissing.png") == nil)
        #expect(MediaExports(dataFolder: nil).take("1\ta.png") == nil)
    }

    @Test func theLimitsHold() throws {
        try put("a.png", "1234")
        try put("b.png", "12345")
        #expect(exports(fileLimit: 4).take("1\tb.png") == nil)
        #expect(exports(fileLimit: 4).take("1\ta.png") != nil)
        let counted = exports(countLimit: 2)
        #expect(counted.take("1\ta.png") != nil)
        #expect(counted.take("1\ta.png") != nil)
        #expect(counted.take("1\ta.png") == nil)
        let total = exports(totalLimit: 8)
        #expect(total.take("1\ta.png") != nil)
        #expect(total.take("1\tb.png") == nil)  // 4 + 5 > 8
        #expect(total.take("1\ta.png") != nil)  // 4 + 4
        #expect(total.take("1\ta.png") == nil)
    }

    @Test func theCopyIsOfTheFileTakenNotOfThePath() throws {
        try put("a.png", "taken")
        let job = try #require(exports().take("1\ta.png"))
        // The app swaps the name for a link meanwhile: the copy is still of what was taken.
        try FileManager.default.removeItem(at: data.appending(path: "a.png"))
        try Data("secret".utf8).write(to: root.appending(path: "secret.png"))
        try FileManager.default.createSymbolicLink(at: data.appending(path: "a.png"),
                                                   withDestinationURL: root.appending(path: "secret.png"))
        let copy = try #require(job.copy(into: copies))
        #expect(try String(contentsOf: copy, encoding: .utf8) == "taken")
    }

    @Test func theCopyHasNoMoreThanTheSizeTaken() throws {
        try put("a.png", "12345")
        let job = try #require(exports().take("1\ta.png"))
        let handle = try FileHandle(forWritingTo: data.appending(path: "a.png"))
        try handle.seekToEnd()
        try handle.write(contentsOf: Data("6789".utf8))
        try handle.close()
        let copy = try #require(job.copy(into: copies))
        #expect(try String(contentsOf: copy, encoding: .utf8) == "12345")
    }

    @Test func aFileThatShrankLeavesNoCopy() throws {
        try put("a.png", "12345")
        let job = try #require(exports().take("1\ta.png"))
        #expect(truncate(data.appending(path: "a.png").path, 2) == 0)
        #expect(job.copy(into: copies) == nil)
        #expect(try FileManager.default.contentsOfDirectory(atPath: copies.path).isEmpty)
    }

    @Test func aCopyThatCannotBeMadeLeavesNothing() throws {
        try put("a.png", "a")
        let job = try #require(exports().take("1\ta.png"))
        #expect(chmod(copies.path, 0o500) == 0)
        defer { chmod(copies.path, 0o700) }
        #expect(job.copy(into: copies) == nil)
        #expect(try FileManager.default.contentsOfDirectory(atPath: copies.path).isEmpty)
    }

    @Test func theCopyWithoutAClone() throws {
        try put("a.png", "12345")
        let exports = exports()
        let plain = try #require(exports.take("1\ta.png")?.copy(into: copies, cloning: false))
        #expect(try String(contentsOf: plain, encoding: .utf8) == "12345")
        let grown = try #require(exports.take("1\ta.png"))
        try put("a.png", "123456789")
        #expect(try String(contentsOf: #require(grown.copy(into: copies, cloning: false)), encoding: .utf8) == "12345")
        let shrunk = try #require(exports.take("1\ta.png"))
        try put("a.png", "12")
        #expect(shrunk.copy(into: copies, cloning: false) == nil)
        #expect(try FileManager.default.contentsOfDirectory(atPath: copies.path).count == 2)
    }

    @Test func aReadOnlyFileIsCopied() throws {
        try put("a.png", "12345")
        #expect(chmod(data.appending(path: "a.png").path, 0o444) == 0)
        let exports = exports()
        let same = try #require(exports.take("1\ta.png")?.copy(into: copies))
        #expect(try String(contentsOf: same, encoding: .utf8) == "12345")
        let grown = try #require(exports.take("1\ta.png"))
        #expect(chmod(data.appending(path: "a.png").path, 0o644) == 0)
        try put("a.png", "123456789")
        #expect(chmod(data.appending(path: "a.png").path, 0o444) == 0)
        #expect(try String(contentsOf: #require(grown.copy(into: copies)), encoding: .utf8) == "12345")
    }

    @Test func sixteenNamesAreTaken() throws {
        let path = Array(repeating: "d", count: 15).joined(separator: "/") + "/a.png"
        try put(path, "a")
        #expect(exports().take("1\t" + path) != nil)
    }

    @Test func aFIFOOnTheWayIsRefused() throws {
        #expect(mkfifo(data.appending(path: "pipe").path, 0o600) == 0)
        #expect(exports().take("1\tpipe/a.png") == nil)  // returns
    }

    @Test func aJobEndsItsDescriptor() throws {
        try put("a.png", "a")
        let exports = exports()
        let copied = try #require(exports.take("1\ta.png"))
        #expect(copied.descriptor >= 0)
        _ = copied.copy(into: copies)
        #expect(copied.descriptor == -1)
        #expect(copied.copy(into: copies) == nil)  // once
    }

    @Test func atMostSoManyFilesAreTakenAtOnce() throws {
        try put("a.png", "a")
        let exports = exports()
        var jobs = (0..<MediaExports.takenLimit).compactMap { _ in exports.take("1\ta.png") }
        #expect(jobs.count == MediaExports.takenLimit)
        #expect(exports.take("1\ta.png") == nil)
        _ = jobs[0].copy(into: copies)  // a copy ends its job
        jobs.append(try #require(exports.take("1\ta.png")))
        #expect(exports.take("1\ta.png") == nil)
        jobs.removeLast()  // so does a job that goes
        jobs.append(try #require(exports.take("1\ta.png")))
        #expect(exports.take("1\ta.png") == nil)
    }

    @Test func aRefusedTakeHoldsNoPlace() throws {
        try put("a.png", "a")
        let exports = exports()
        for _ in 0...MediaExports.takenLimit { #expect(exports.take("1\tmissing.png") == nil) }
        #expect(exports.take("1\ta.png") != nil)
    }

    @Test func theFoldersOnTheWayAreClosed() throws {
        try put("a/b/c/a.png", "a")
        let exports = exports(countLimit: .max)
        // The process's descriptors, which the other suites change by a few at most meanwhile.
        func open() -> Int { (try? FileManager.default.contentsOfDirectory(atPath: "/dev/fd").count) ?? 0 }
        let before = open()
        for _ in 0..<1000 { #expect(exports.take("1\ta/b/c/a.png") != nil) }  // each job goes at once
        #expect(open() - before < 100)  // not 4000
    }

    @Test func aRefusedTakeOfAFileClosesIt() throws {
        try put("a.png", "too big")
        let exports = exports(fileLimit: 1)
        func open() -> Int { (try? FileManager.default.contentsOfDirectory(atPath: "/dev/fd").count) ?? 0 }
        let before = open()
        for _ in 0..<1000 { #expect(exports.take("1\ta.png") == nil) }
        #expect(open() - before < 100)  // not 1000
    }

    @Test func aGrownCloneThatCannotBeCutIsCopiedInstead() throws {
        try put("a.png", "12345")
        let job = try #require(exports().take("1\ta.png"))
        try put("a.png", "123456789")
        let copy = try #require(job.copy(into: copies, cutting: { _, _ in -1 }))
        #expect(try String(contentsOf: copy, encoding: .utf8) == "12345")
        #expect(try FileManager.default.contentsOfDirectory(atPath: copy.deletingLastPathComponent().path) == ["a.png"])
    }

    @Test func theNameShownHidesInvisibleCharacters() throws {
        func shown(_ name: String) throws -> String {
            try put(name, "a")
            return try #require(exports().take("1\t" + name)).shownName
        }
        #expect(try shown("Hello world.png") == "Hello world.png")
        #expect(try shown("gnp\u{202E}.exe") == "gnp.exe")
        #expect(try shown("a\u{061C}b\u{E0041}.jpg") == "ab.jpg")
        #expect(try shown("\u{200B}.png") == "Export.png")
        #expect(try shown("\u{FEFF}") == "Export")
        #expect(try shown("a\u{1}b.png") == "ab.png")  // control
        #expect(try shown("a\u{E000}b.png") == "ab.png")  // private use
        // APFS refuses a name with an unassigned code point, so this one is a job of its own.
        let unassigned = MediaExportJob(source: -1, kind: .image, name: "a\u{0378}b.png", size: 0, taken: Taken())
        #expect(unassigned.shownName == "ab.png")
        #expect(try shown("a\u{2028}b\u{2029}c.png") == "abc.png")  // line and paragraph separators
    }
}
