import APKKit
import APKTestSupport
import Darwin
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0050 decision 2: at every start of an app whose fast-follow and on-demand pack slices the library keeps, they lie
/// as read-only clones in local_testing of its external files folder, /sdcard/Android/data/<package>/files/local_testing,
/// where Google's Play libraries look once the meta-data local_testing_dir names it. The guest can write that folder, so
/// the layout never follows a link the guest made: what lies outside stays as it is.
@Suite("Pack layout")
struct PackLayoutTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Packs = PackImportTests
    static let codePath = "/data/app/~~AAAA/org.example.sets-BBBB=="

    /// An entry with the packs level2 (two slices) and level3 kept, and its data folders, as the start makes them, or
    /// none when not `prepared`.
    static func entryWithPacks(_ sandbox: Sandbox, prepared: Bool = true) async throws -> LibraryEntry {
        let unit = try SetImportTests.unit(["game.apks": Packs.apks()], in: sandbox.paths)
        let entry = try #require(try await Library(paths: sandbox.paths).importUnit(unit).first?.entry)
        try #require(entry.packs?.count == 3)
        if prepared {
            try sandbox.paths.prepareData(for: entry.id)
        } else if sandbox.exists(sandbox.paths.dataDirectory(for: entry.id)) {
            try FileManager.default.removeItem(at: sandbox.paths.dataDirectory(for: entry.id))
        }
        return entry
    }

    static func external(_ entry: LibraryEntry, _ sandbox: Sandbox) -> URL {
        sandbox.paths.dataDirectory(for: entry.id).appendingPathComponent("external", isDirectory: true)
    }

    static func localTesting(_ entry: LibraryEntry, _ sandbox: Sandbox) -> URL {
        external(entry, sandbox).appendingPathComponent("files/local_testing", isDirectory: true)
    }

    /// A folder beside the library with one file of the user's, which the guest's links point at.
    static func outside(_ sandbox: Sandbox) throws -> URL {
        let folder = sandbox.root.appendingPathComponent("outside", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try Data("the user's".utf8).write(to: folder.appendingPathComponent("keep"))
        try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: folder.appendingPathComponent("keep").path)
        return folder
    }

    static func contents(_ folder: URL) throws -> [String] {
        try FileManager.default.contentsOfDirectory(atPath: folder.path).sorted()
    }

    /// Whether `url` itself is a folder, not a link to one.
    static func isFolder(_ url: URL) -> Bool {
        var info = stat()
        return lstat(url.path, &info) == 0 && info.st_mode & S_IFMT == S_IFDIR
    }

    /// local_testing is a folder of its own that holds the version's slices and nothing else, each a read-only file
    /// with its original's bytes.
    static func expectSlices(of entry: LibraryEntry, in sandbox: Sandbox) throws {
        let folder = localTesting(entry, sandbox)
        #expect(isFolder(folder))
        #expect(try contents(folder) == ["level2-astc.apk", "level2-master.apk", "level3-master.apk"])
        for name in try contents(folder) {
            let clone = folder.appendingPathComponent(name)
            let original = Packs.packsFolder(entry, sandbox.paths).appendingPathComponent(name)
            #expect(try Data(contentsOf: clone) == Data(contentsOf: original))
            #expect(try LibraryTests.permissions(clone) == 0o444)
            #expect(try FileManager.default.attributesOfItem(atPath: clone.path)[.type] as? FileAttributeType == .typeRegular)
        }
    }

    /// Where the guest's links pointed, nothing changed: the folder holds its one file, with its bytes and mode.
    static func expectUntouched(_ outside: URL) throws {
        #expect(try contents(outside) == ["keep"])
        #expect(try Data(contentsOf: outside.appendingPathComponent("keep")) == Data("the user's".utf8))
        #expect(try LibraryTests.permissions(outside.appendingPathComponent("keep")) == 0o644)
    }

    @Test func aStartLaysOutThePackSlicesAsReadOnlyClones() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
        // Left out of backups, as packs/ is (decision 9).
        #expect(XAPKImportTests.isMarkedExcluded(Self.localTesting(entry, sandbox)))
    }

    /// prepareJavaStart lays them out without a start before it that made the app's data folders.
    @Test func aFirstStartMakesTheAppsFolders() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox, prepared: false)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
    }

    /// A clone is read-only whatever mode its original has.
    @Test func aWritableSliceStillGivesAReadOnlyClone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)
        let original = Packs.packsFolder(entry, sandbox.paths).appendingPathComponent("level3-master.apk")
        try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: original.path)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
    }

    /// What the guest left in local_testing goes at the next start, also from a folder it made read-only, and a slice
    /// it made writable and changed comes back as it was; the app's own files beside local_testing stay.
    @Test func eachStartLaysThemOutAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)
        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)
        let folder = Self.localTesting(entry, sandbox)
        let slice = folder.appendingPathComponent("level3-master.apk")
        try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: slice.path)
        try Data("written by the guest".utf8).write(to: slice)
        try Data("left".utf8).write(to: folder.appendingPathComponent("extra.apk"))
        let made = folder.appendingPathComponent("made/deeper", isDirectory: true)
        try FileManager.default.createDirectory(at: made, withIntermediateDirectories: true)
        try Data("left".utf8).write(to: made.appendingPathComponent("file"))
        for readOnly in [made, folder] {
            try FileManager.default.setAttributes([.posixPermissions: 0o555], ofItemAtPath: readOnly.path)
        }
        let own = folder.deletingLastPathComponent().appendingPathComponent("save.dat")
        try Data("the app's".utf8).write(to: own)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
        #expect(try Data(contentsOf: own) == Data("the app's".utf8))
    }

    /// An app without kept packs gets no local_testing, and no files folder made for it.
    @Test func anAppWithoutPacksGetsNoLocalTesting() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        try sandbox.paths.prepareData(for: entry.id)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        #expect(!sandbox.exists(Self.external(entry, sandbox).appendingPathComponent("files")))
    }

    /// A link the guest put in place of the app's own folders, external or files, stops the start, and nothing is made
    /// or removed where it points.
    @Test(arguments: ["external", "external/files"])
    func aLinkInPlaceOfTheAppsFoldersStopsTheStart(_ path: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)
        let outside = try Self.outside(sandbox)
        let link = sandbox.paths.dataDirectory(for: entry.id).appendingPathComponent(path)
        if sandbox.exists(link) {
            try FileManager.default.removeItem(at: link)
        }
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: outside)

        #expect(throws: (any Error).self) {
            _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)
        }

        try Self.expectUntouched(outside)
    }

    /// A link or a file the guest put in place of local_testing, Minato's folder, goes, and a folder takes its place;
    /// where the link points stays as it was.
    @Test(arguments: [true, false])
    func aLinkOrAFileInPlaceOfLocalTestingGoes(_ link: Bool) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)
        let outside = try Self.outside(sandbox)
        let folder = Self.localTesting(entry, sandbox)
        try FileManager.default.createDirectory(at: folder.deletingLastPathComponent(), withIntermediateDirectories: true)
        if link {
            try FileManager.default.createSymbolicLink(at: folder, withDestinationURL: outside)
        } else {
            try Data("a file".utf8).write(to: folder)
        }

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
        try Self.expectUntouched(outside)
    }

    /// Links the guest put into local_testing go as links: one under a slice's name, one to a folder, and one in a
    /// folder of the guest's; the file and the folder they point at stay as they were.
    @Test func linksInLocalTestingGoAsLinks() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Self.entryWithPacks(sandbox)
        let outside = try Self.outside(sandbox)
        let keep = outside.appendingPathComponent("keep")
        let folder = Self.localTesting(entry, sandbox)
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("made"), withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("level2-master.apk"), withDestinationURL: keep)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("away"), withDestinationURL: outside)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("made/keep"), withDestinationURL: keep)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: Self.codePath)

        try Self.expectSlices(of: entry, in: sandbox)
        try Self.expectUntouched(outside)
    }
}
