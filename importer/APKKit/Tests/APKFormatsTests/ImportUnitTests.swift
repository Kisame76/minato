import APKFormats
import Foundation
import Testing

/// What a picked folder gives to its import unit (ADR-0048, decision 1).
@Suite("Import units")
struct ImportUnitTests {
    final class Folder {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("unit-\(UUID().uuidString)", isDirectory: true)

        init(_ files: [String]) throws {
            for file in files {
                let path = url.appendingPathComponent(file)
                try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
                try Data("x".utf8).write(to: path)
            }
        }

        deinit { try? FileManager.default.removeItem(at: url) }

        /// Paths below the folder; the temporary folder lies behind the /var → /private/var link.
        func names(_ urls: [URL]) -> [String] {
            let base = url.resolvingSymlinksInPath().path
            return urls.map { String($0.resolvingSymlinksInPath().path.dropFirst(base.count + 1)) }
        }
    }

    @Test func aFolderGivesItsApksAndObbs() throws {
        let folder = try Folder(["split_config.de.apk", "base.apk", "main.1.org.example.obb", "readme.txt", ".DS_Store",
                                 "sub/other.apk", "UPPER.APK"])
        #expect(folder.names(try ImportUnit.files(inFolder: folder.url))
            == ["UPPER.APK", "base.apk", "main.1.org.example.obb", "split_config.de.apk"])
    }

    @Test func aFolderWithNothingButOneFolderGivesThatFoldersFiles() throws {
        // As Android's directory parser (ApkLiteParseUtils.java:179-181); hidden files such as .DS_Store do not count.
        let folder = try Folder([".DS_Store", "app/inner/base.apk", "app/inner/split_config.de.apk"])
        #expect(folder.names(try ImportUnit.files(inFolder: folder.url)) == ["app/inner/base.apk", "app/inner/split_config.de.apk"])
    }

    @Test func aFolderWithAFolderAndAFileGivesOnlyItsOwnFiles() throws {
        let folder = try Folder(["base.apk", "more/split_config.de.apk"])
        #expect(folder.names(try ImportUnit.files(inFolder: folder.url)) == ["base.apk"])
    }

    @Test func anEmptyFolderGivesNothing() throws {
        let folder = try Folder([])
        try FileManager.default.createDirectory(at: folder.url, withIntermediateDirectories: true)
        #expect(try ImportUnit.files(inFolder: folder.url).isEmpty)
    }

    @Test func linksAreNotFollowed() throws {
        let elsewhere = try Folder(["split_config.de.apk", "inner/base.apk"])
        let folder = try Folder(["base.apk"])
        try FileManager.default.createSymbolicLink(at: folder.url.appendingPathComponent("split_config.de.apk"),
                                                   withDestinationURL: elsewhere.url.appendingPathComponent("split_config.de.apk"))
        #expect(folder.names(try ImportUnit.files(inFolder: folder.url)) == ["base.apk"])
        // Nor a folder's only entry when it links to a folder.
        let linked = try Folder([])
        try FileManager.default.createDirectory(at: linked.url, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: linked.url.appendingPathComponent("app"),
                                                   withDestinationURL: elsewhere.url.appendingPathComponent("inner"))
        #expect(try ImportUnit.files(inFolder: linked.url).isEmpty)
    }

    @Test func foldersNestedPastTheLimitGiveNothing() throws {
        let nested = { (depth: Int) in Array(repeating: "d", count: depth).joined(separator: "/") + "/base.apk" }
        let deepest = try Folder([nested(ImportUnit.maxDepth - 1)])
        #expect(try ImportUnit.files(inFolder: deepest.url).count == 1)
        let tooDeep = try Folder([nested(ImportUnit.maxDepth)])
        #expect(try ImportUnit.files(inFolder: tooDeep.url).isEmpty)
    }

    @Test func aFolderOverTheCapIsRefused() throws {
        let folder = try Folder((0...ImportUnit.maxFiles).map { "split_\($0).apk" })
        #expect(throws: ImportUnitError.tooManyFiles(ImportUnit.maxFiles + 1)) {
            try ImportUnit.files(inFolder: folder.url)
        }
    }
}
