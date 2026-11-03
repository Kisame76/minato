import APKTestSupport
import Darwin
import Foundation
import Testing

@testable import APKLibrary

/// Settings › Storage (docs/plans/2026-09-24-app-navigation-design.md, section 6): each app's space, the app and its
/// data apart, as the iPhone Storage screen shows App Size and Documents & Data, largest first.
@Suite("Storage use")
struct StorageUseTests {
    typealias Sandbox = LibraryTests.Sandbox

    private func library(_ sandbox: Sandbox, _ packages: [String]) async throws -> Library {
        let library = try Library(paths: sandbox.paths)
        for package in packages {
            _ = try await library.importFile(sandbox.file(SampleAPK.make(package: package, label: package),
                                                          named: "\(package).apk"))
        }
        return library
    }

    private func write(_ bytes: Int, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(repeating: 7, count: bytes).write(to: url)
    }

    /// What the disk gives a file, as lstat counts it.
    private func allocated(_ url: URL) -> UInt64 {
        var status = stat()
        return lstat(url.path, &status) == 0 ? UInt64(status.st_blocks) * 512 : 0
    }

    @Test func eachAppsDataIsWhatItKeepsAndWhatItsStartsMade() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app"])
        let paths = sandbox.paths
        let files = [
            paths.dataDirectory(for: "a.app").appending(path: "data/files/save"),
            paths.dataDirectory(for: "a.app").appending(path: "obb/main.1.a.app.obb"),
            paths.writeBackDirectory(for: "a.app").appending(path: "record"),
            paths.guestDataDirectory(for: "a.app").appending(path: "dalvik-cache/arm64/base.art"),
            SessionLog(folder: paths.sessionLogDirectory).keptFile(for: "a.app"),
        ]
        for (index, file) in files.enumerated() { try write(100_000 * (index + 1), to: file) }

        let uses = await library.storage()
        let a = try #require(uses.first { $0.id == "a.app" })
        let b = try #require(uses.first { $0.id == "b.app" })
        let written: UInt64 = files.map(allocated).reduce(0, +)
        #expect(a.data >= written)
        #expect(a.data < written + 64 * 1024)
        #expect(b.data < 64 * 1024)
        // The installed files are the app's part, its data none of it.
        #expect(a.app > 0 && a.app == b.app)
        #expect(a.total == a.app + a.data)
    }

    @Test func theInstalledFilesAndTheirConvertedCodeAreTheApps() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app"])
        let entry = try #require(await library.entries.first)
        let before = try #require(await library.storage().first)
        let converted = sandbox.paths.libraryDirectory(for: entry).appending(path: "arm64-v8a/libmain.so.dylib")
        try write(300_000, to: converted)
        let after = try #require(await library.storage().first)
        #expect(after.app == before.app + allocated(converted))
        #expect(after.data == before.data)
    }

    @Test func theLargestComesFirst() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app", "b.app", "c.app"])
        try write(200_000, to: sandbox.paths.dataDirectory(for: "b.app").appending(path: "data/files/f"))
        try write(900_000, to: sandbox.paths.dataDirectory(for: "c.app").appending(path: "data/files/f"))
        #expect(await library.storage().map(\.id) == ["c.app", "b.app", "a.app"])
    }

    @Test func aLinkCountsAsTheLinkAndAFileLinkedTwiceOnce() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try await library(sandbox, ["a.app"])
        let before = try #require(await library.storage().first)
        let outside = sandbox.root.appending(path: "outside/big")
        try write(2_000_000, to: outside)
        let files = sandbox.paths.dataDirectory(for: "a.app").appending(path: "data/files")
        try FileManager.default.createDirectory(at: files, withIntermediateDirectories: true)
        // A guest's link to a file or a folder outside its data is never followed.
        try FileManager.default.createSymbolicLink(at: files.appending(path: "file-link"), withDestinationURL: outside)
        try FileManager.default.createSymbolicLink(at: files.appending(path: "folder-link"),
                                                   withDestinationURL: outside.deletingLastPathComponent())
        let once = files.appending(path: "once")
        try write(500_000, to: once)
        try FileManager.default.linkItem(at: once, to: files.appending(path: "twice"))
        let after = try #require(await library.storage().first)
        #expect(after.data >= before.data + allocated(once))
        #expect(after.data < before.data + allocated(once) + 64 * 1024)
    }
}
