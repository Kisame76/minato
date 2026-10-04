import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// One pick, several files or a folder, taken over as one unit (ADR-0048, decision 1; step b4, test 1).
@Suite("Import units")
struct ImportUnitLibraryTests {
    typealias Sandbox = LibraryTests.Sandbox

    /// What incoming/ holds: each entry's name, and for a folder the names in it.
    static func incoming(_ paths: LibraryPaths) throws -> [String: [String]] {
        var found: [String: [String]] = [:]
        let fileManager = FileManager.default
        for name in (try? fileManager.contentsOfDirectory(atPath: paths.incoming.path)) ?? [] {
            var isDirectory: ObjCBool = false
            let path = paths.incoming.appendingPathComponent(name).path
            _ = fileManager.fileExists(atPath: path, isDirectory: &isDirectory)
            found[name] = isDirectory.boolValue ? try fileManager.contentsOfDirectory(atPath: path).sorted() : []
        }
        return found
    }

    /// The scans a test ran while a pick was taken over.
    actor Scans {
        var seen: [[URL]] = []
        func add(_ urls: [URL]) { seen.append(urls) }
    }

    @Test func aPickWaitsAsOneUnitInItsOwnFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let one = try sandbox.file(SampleAPK.make(), named: "game.apk")
        let two = try sandbox.file(SampleAPK.make(package: "org.example.other"), named: "game.apk",
                                   in: sandbox.root.appendingPathComponent("iCloud"))
        let library = try Library(paths: sandbox.paths)

        let unit = try await library.adopt([one, two])

        #expect(LibraryTests.resolved([unit.deletingLastPathComponent()]) == LibraryTests.resolved([sandbox.paths.incoming]))
        #expect(try Self.incoming(sandbox.paths) == [unit.lastPathComponent: ["game 2.apk", "game.apk"]])
        #expect(try Data(contentsOf: unit.appendingPathComponent("game.apk")) == SampleAPK.make())
        #expect(try Data(contentsOf: unit.appendingPathComponent("game 2.apk")) == SampleAPK.make(package: "org.example.other"))
        #expect(sandbox.exists(one) && sandbox.exists(two))
        #expect(LibraryTests.resolved(try await library.scan())
            == LibraryTests.resolved([unit.appendingPathComponent("game 2.apk"), unit.appendingPathComponent("game.apk")]))
    }

    @Test func aScanWhileAPickIsTakenOverSeesNothing() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let picked = try (1...3).map { try sandbox.file(SampleAPK.make(versionCode: Int32($0)), named: "v\($0).apk") }
        let paths = sandbox.paths
        let library = try Library(paths: paths)
        let scans = Scans()
        // LibraryModel scans with its Library; "Open with" during a session takes a file over with a Library of its own.
        await library.runBetweenClones {
            await scans.add(try await library.scan())
            await scans.add(try await Library(paths: paths).scan())
        }

        let unit = try await library.adopt(picked)

        #expect(await scans.seen.count == 6)
        #expect(await scans.seen.allSatisfy(\.isEmpty))
        #expect(LibraryTests.resolved(try await library.scan())
            == LibraryTests.resolved(["v1.apk", "v2.apk", "v3.apk"].map { unit.appendingPathComponent($0) }))
    }

    @Test func anInterruptedPickLeavesNothing() async throws {
        struct Interruption: Error {}
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let picked = try (1...2).map { try sandbox.file(SampleAPK.make(versionCode: Int32($0)), named: "v\($0).apk") }
        let library = try Library(paths: sandbox.paths)
        await library.runBetweenClones { throw Interruption() }

        await #expect(throws: Interruption.self) { try await library.adopt(picked) }

        #expect(try Self.incoming(sandbox.paths).isEmpty)
        #expect(try await library.scan().isEmpty)
        #expect(picked.allSatisfy(sandbox.exists))
    }

    @Test func aPickAnEarlierLaunchDidNotFinishGoesAtLaunch() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // The app stopped while it took a pick over; the picks it finished stay.
        let partial = sandbox.paths.incoming.appendingPathComponent(UUID().uuidString + ".partial", isDirectory: true)
        _ = try sandbox.file(SampleAPK.make(), named: "game.apk", in: partial)
        let unit = sandbox.paths.incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let finished = try sandbox.file(SampleAPK.make(package: "org.example.finished"), named: "finished.apk", in: unit)
        let flat = try sandbox.file(SampleAPK.make(package: "org.example.flat"), named: "flat.apk", in: sandbox.paths.incoming)

        let library = try Library(paths: sandbox.paths)

        #expect(try Self.incoming(sandbox.paths) == [unit.lastPathComponent: ["finished.apk"], "flat.apk": []])
        #expect(Set(LibraryTests.resolved(try await library.scan())) == Set(LibraryTests.resolved([finished, flat])))
    }

    @Test func anEmptiedUnitGoesAtTheNextScan() async throws {
        // Its last file was imported, and the app stopped before the folder went.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let unit = sandbox.paths.incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: unit, withIntermediateDirectories: true)
        let library = try Library(paths: sandbox.paths)

        #expect(try await library.scan().isEmpty)
        #expect(try Self.incoming(sandbox.paths).isEmpty)
    }

    @Test func aFileLyingFlatInIncomingIsAUnitOfItsOwn() async throws {
        // Older builds took a file over flat, and tools/session-check.sh and tools/update-check.sh still drop one there.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let flat = try sandbox.file(SampleAPK.make(), named: "game.apk", in: sandbox.paths.incoming)
        _ = try sandbox.file(Data("notes".utf8), named: "notes.txt", in: sandbox.paths.incoming)
        let library = try Library(paths: sandbox.paths)
        let unit = try await library.adopt([try sandbox.file(SampleAPK.make(package: "org.example.later"), named: "later.apk")])

        #expect(LibraryTests.resolved(try await library.scan())
            == LibraryTests.resolved([flat, unit.appendingPathComponent("later.apk")]))
        _ = try await library.importFile(flat)
        #expect(!sandbox.exists(flat))
        #expect(await library.entries.map(\.id) == ["org.example.game"])
    }

    @Test func unitsWaitOldestFirst() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        // Five picks: the order of their folders' random names matches theirs only once in 120 runs.
        var picked: [URL] = []
        for name in ["e", "d", "c", "b", "a"] {
            picked.append(try await library.adopt(sandbox.file(SampleAPK.make(package: "org.example.\(name)"), named: "\(name).apk")))
        }

        #expect(LibraryTests.resolved(try await library.scan()) == LibraryTests.resolved(picked))
    }

    @Test func importingAUnitsLastFileRemovesTheUnit() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try await library.adopt([try sandbox.file(SampleAPK.make(), named: "game.apk"),
                                            try sandbox.file(Data("junk".utf8), named: "junk.apk")])

        _ = try await library.importFile(unit.appendingPathComponent("game.apk"))
        #expect(try Self.incoming(sandbox.paths) == [unit.lastPathComponent: ["junk.apk"]])
        _ = try await library.importFile(unit.appendingPathComponent("junk.apk"))
        #expect(try Self.incoming(sandbox.paths).isEmpty)
    }

    @Test func aFolderWithOnlyASubfolderGivesThatSubfoldersFiles() async throws {
        // As Android's directory parser does (ApkLiteParseUtils.java:179-181).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let folder = sandbox.root.appendingPathComponent("Picked", isDirectory: true)
        let inner = folder.appendingPathComponent("game", isDirectory: true)
        _ = try sandbox.file(SampleAPK.make(), named: "base.apk", in: inner)
        _ = try sandbox.file(Data("split".utf8), named: "split_config.de.apk", in: inner)
        _ = try sandbox.file(Data("obb".utf8), named: "main.1.org.example.game.obb", in: inner)
        let library = try Library(paths: sandbox.paths)

        let unit = try await library.adopt([folder])

        #expect(try Self.incoming(sandbox.paths)
            == [unit.lastPathComponent: ["base.apk", "main.1.org.example.game.obb", "split_config.de.apk"]])
        // One APK at a time until step b5 imports whole units; the OBB waits for ADR-0049.
        #expect(LibraryTests.resolved(try await library.scan())
            == LibraryTests.resolved(["base.apk", "split_config.de.apk"].map { unit.appendingPathComponent($0) }))
    }

    @Test func aPickOverTheCapIsRefused() async throws {
        // At most ImportUnit.maxFiles over the whole pick, not only per folder.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let folders = ["One", "Two"].map { sandbox.root.appendingPathComponent($0, isDirectory: true) }
        for (folder, count) in zip(folders, [256, ImportUnit.maxFiles - 255]) {
            for index in 0..<count { _ = try sandbox.file(Data("x".utf8), named: "\(index).apk", in: folder) }
        }
        let library = try Library(paths: sandbox.paths)

        await #expect(throws: ImportUnitError.tooManyFiles(ImportUnit.maxFiles + 1)) { try await library.adopt(folders) }
        #expect(try Self.incoming(sandbox.paths).isEmpty)
        try FileManager.default.removeItem(at: folders[1].appendingPathComponent("0.apk"))
        let unit = try await library.adopt(folders)
        #expect(try Self.incoming(sandbox.paths)[unit.lastPathComponent]?.count == ImportUnit.maxFiles)
    }

    @Test func linksAreNeverTakenOver() async throws {
        // Nothing in incoming/ may point outside the pick, e.g. into an app's data.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let elsewhere = try sandbox.file(SampleAPK.make(package: "org.example.elsewhere"), named: "elsewhere.apk")
        let folder = sandbox.root.appendingPathComponent("Picked", isDirectory: true)
        _ = try sandbox.file(SampleAPK.make(), named: "base.apk", in: folder)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("linked.apk"), withDestinationURL: elsewhere)
        let pickedLink = sandbox.root.appendingPathComponent("picked-link.apk")
        try FileManager.default.createSymbolicLink(at: pickedLink, withDestinationURL: elsewhere)
        let library = try Library(paths: sandbox.paths)

        let unit = try await library.adopt([folder, pickedLink])

        #expect(try Self.incoming(sandbox.paths) == [unit.lastPathComponent: ["base.apk"]])
        await #expect(throws: LibraryError.nothingToImport) { try await library.adopt([pickedLink]) }
    }

    @Test func aPickWithNothingToImportIsRefused() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let empty = sandbox.root.appendingPathComponent("Empty", isDirectory: true)
        try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        _ = try sandbox.file(Data("notes".utf8), named: "readme.txt", in: empty)
        let library = try Library(paths: sandbox.paths)

        await #expect(throws: LibraryError.nothingToImport) { try await library.adopt([empty]) }
        #expect(try Self.incoming(sandbox.paths).isEmpty)
    }
}
