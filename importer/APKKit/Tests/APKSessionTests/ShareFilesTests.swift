import Darwin
import Foundation
import Testing

@testable import APKSession

/// ADR-0062, decision 3: the host copies a share's files out of the app's share folder, which the app can fill with
/// links, folders and FIFOs, through descriptors only.
@Suite("ShareFiles")
final class ShareFilesTests {
    let root: URL
    let data: URL
    let destination: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "share-files-\(UUID().uuidString)")
        data = root.appending(path: "data")
        destination = root.appending(path: "copies")
        try FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
    }

    deinit { try? FileManager.default.removeItem(at: root) }

    var share: URL { data.appending(path: ".minato/share/7") }

    func put(_ name: String, _ text: String) throws {
        try FileManager.default.createDirectory(at: share, withIntermediateDirectories: true)
        try Data(text.utf8).write(to: share.appending(path: name))
    }

    func copies() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: destination.path)) ?? []).sorted()
    }

    func copy(_ names: String..., fileLimit: Int64 = ShareFiles.fileLimit, totalLimit: Int64 = ShareFiles.totalLimit)
        -> [URL]? {
        ShareFiles.copy(dataFolder: data, id: 7, files: names.map { ShareFile(name: $0, type: "text/plain") },
                        into: destination, fileLimit: fileLimit, totalLimit: totalLimit)
    }

    @Test func theFilesAreCopiedInTheRequestsOrder() throws {
        try put("b.txt", "second")
        try put("a b.txt", "first")
        let urls = try #require(copy("a b.txt", "b.txt"))
        #expect(urls.map(\.lastPathComponent) == ["a b.txt", "b.txt"])
        #expect(try String(contentsOf: urls[0], encoding: .utf8) == "first")
        #expect(try String(contentsOf: urls[1], encoding: .utf8) == "second")
        #expect(try String(contentsOf: share.appending(path: "a b.txt"), encoding: .utf8) == "first")  // untouched
    }

    @Test func aShareWithoutFilesCopiesNothing() {
        #expect(ShareFiles.copy(dataFolder: data, id: 7, files: [], into: destination) == [])
    }

    @Test func aLinkOnTheWayRefusesTheShare() throws {
        // The app's link at each component, to a folder with the same file below it.
        let elsewhere = root.appending(path: "elsewhere")
        try FileManager.default.createDirectory(at: elsewhere.appending(path: "share/7"), withIntermediateDirectories: true)
        try Data("secret".utf8).write(to: elsewhere.appending(path: "share/7/a.txt"))
        try FileManager.default.createDirectory(at: elsewhere.appending(path: "7"), withIntermediateDirectories: true)
        try Data("secret".utf8).write(to: elsewhere.appending(path: "7/a.txt"))
        try FileManager.default.createDirectory(at: elsewhere.appending(path: "x"), withIntermediateDirectories: true)
        try Data("secret".utf8).write(to: elsewhere.appending(path: "x/a.txt"))
        let minato = data.appending(path: ".minato")
        try FileManager.default.createSymbolicLink(at: minato, withDestinationURL: elsewhere)
        #expect(copy("a.txt") == nil)
        try FileManager.default.removeItem(at: minato)
        try FileManager.default.createDirectory(at: minato, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: minato.appending(path: "share"), withDestinationURL: elsewhere)
        #expect(copy("a.txt") == nil)
        try FileManager.default.removeItem(at: minato.appending(path: "share"))
        try FileManager.default.createDirectory(at: minato.appending(path: "share"), withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: share, withDestinationURL: elsewhere.appending(path: "x"))
        #expect(copy("a.txt") == nil)
        #expect(copies().isEmpty)
    }

    @Test func aLinkAFolderOrAFIFOAsAFileRefusesTheShare() throws {
        try put("a.txt", "a")
        try Data("secret".utf8).write(to: root.appending(path: "secret.txt"))
        try FileManager.default.createSymbolicLink(at: share.appending(path: "link.txt"),
                                                   withDestinationURL: root.appending(path: "secret.txt"))
        #expect(copy("a.txt", "link.txt") == nil)
        try FileManager.default.createDirectory(at: share.appending(path: "folder"), withIntermediateDirectories: true)
        #expect(copy("a.txt", "folder") == nil)
        #expect(mkfifo(share.appending(path: "fifo").path, 0o600) == 0)
        #expect(copy("a.txt", "fifo") == nil)  // without waiting for a writer
        #expect(copy("a.txt", "missing.txt") == nil)
        #expect(copies().isEmpty, "the copies made before the refusal are gone")
    }

    @Test func aFileOverTheLimitRefusesTheShare() throws {
        try put("a.txt", "12345")
        #expect(copy("a.txt", fileLimit: 4) == nil)
        #expect(copy("a.txt", fileLimit: 5)?.count == 1)
    }

    @Test func theFilesTogetherOverTheLimitRefuseTheShare() throws {
        try put("a.txt", "123")
        try put("b.txt", "456")
        #expect(copy("a.txt", "b.txt", totalLimit: 5) == nil)
        #expect(copies().isEmpty)
        #expect(copy("a.txt", "b.txt", totalLimit: 6)?.count == 2)
    }

    @Test func aNameTheDestinationHasRefusesTheShare() throws {
        try put("a.txt", "new")
        try Data("old".utf8).write(to: destination.appending(path: "a.txt"))
        #expect(copy("a.txt") == nil)
        #expect(try String(contentsOf: destination.appending(path: "a.txt"), encoding: .utf8) == "old")
    }

    @Test func aHardLinkRefusesTheShare() throws {
        try put("a.txt", "a")
        try Data("secret".utf8).write(to: root.appending(path: "secret.txt"))
        #expect(link(root.appending(path: "secret.txt").path, share.appending(path: "linked.txt").path) == 0)
        #expect(copy("a.txt", "linked.txt") == nil)
        #expect(copies().isEmpty)
    }

    @Test func twoNamesTheFileSystemTakesForOneRefuseTheShare() throws {
        try put("photo.txt", "one")
        // Only on a volume that ignores case, as the Mac's and the iPhone's data volume do.
        guard FileManager.default.fileExists(atPath: share.appending(path: "PHOTO.txt").path) else { return }
        #expect(copy("photo.txt", "PHOTO.txt") == nil)
        #expect(copies().isEmpty)
    }
}
