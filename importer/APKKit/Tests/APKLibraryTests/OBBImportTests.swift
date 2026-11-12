import APKFormats
import APKKit
import APKTestSupport
import CryptoKit
import Foundation
import Synchronization
import Testing

@testable import APKLibrary

/// A single OBB the user picks (ADR-0049 decision 5): it goes to the app its name gives, whatever the version in it, or
/// waits until the user names the app, and is then named for that app; it replaces the installed OBB of its kind, newer
/// or older and under its own name too, as the pick is the user's own act.
@Suite("OBB import")
struct OBBImportTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    static let package = SplitAPK.package

    static func name(_ kind: String, _ version: Int, _ package: String = package) -> String {
        "\(kind).\(version).\(package).obb"
    }

    static func data(_ text: String) -> Data { Data(text.utf8) }

    static func obb(_ name: String, _ data: Data) -> PackageFile {
        PackageFile(fileName: name, splitName: nil, size: UInt64(data.count), sha256: Sets.sha256(data))
    }

    static func obbFolder(_ paths: LibraryPaths, _ package: String = package) -> URL {
        paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true)
    }

    static func obbFiles(_ paths: LibraryPaths, _ package: String = package) throws -> [String] {
        try FileManager.default.contentsOfDirectory(atPath: obbFolder(paths, package).path).sorted()
    }

    static func content(_ paths: LibraryPaths, _ name: String) throws -> Data {
        try Data(contentsOf: obbFolder(paths).appendingPathComponent(name))
    }

    /// The app of `package`, installed from its base APK.
    static func install(_ library: Library, _ sandbox: Sandbox, _ apk: Data = Sets.base) async throws -> LibraryEntry {
        try #require(try await library.importUnit(Sets.unit(["base.apk": apk], in: sandbox.paths)).first?.entry)
    }

    // MARK: - By its name

    @Test func aPickedOBBGoesToTheAppItsNameGivesWhateverItsVersion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        let unit = try Sets.unit([Self.name("main", 5): Self.data("main 5")], in: sandbox.paths)
        #expect(try await library.scanUnits().map(\.lastPathComponent) == [unit.lastPathComponent])

        let results = try await library.importUnit(unit)

        let obb = Self.obb(Self.name("main", 5), Self.data("main 5"))
        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [obb])
        #expect(results.first?.replacedOBBs == [])
        #expect(await library.entries.first?.obbs == [obb])
        #expect(try Self.content(sandbox.paths, Self.name("main", 5)) == Self.data("main 5"))
        #expect(!sandbox.exists(unit))
    }

    @Test func aPickedOBBReplacesItsKindNewerOrOlder() async throws {
        // Decision 5: the pick is the user's own act, so main 7 goes over main 9; the patch stays.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        _ = try await library.importUnit(Sets.unit([Self.name("main", 9): Self.data("main 9"),
                                                    Self.name("patch", 9): Self.data("patch 9")], in: sandbox.paths))

        let results = try await library.importUnit(Sets.unit([Self.name("main", 7): Self.data("main 7")], in: sandbox.paths))

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 7), Self.data("main 7"))])
        #expect(results.first?.replacedOBBs == [Self.name("main", 9)])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 7), Self.name("patch", 9)])
        #expect(await library.entries.first?.obbs.map(\.fileName).sorted() == [Self.name("main", 7), Self.name("patch", 9)])
    }

    @Test func theSameBytesMoveNothingAndOtherBytesUnderItsNameReplaceWithoutAQuestion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        _ = try await library.importUnit(Sets.unit([Self.name("main", 7): Self.data("main 7")], in: sandbox.paths))
        let file = Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 7))
        let inode = try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int

        let same = try Sets.unit([Self.name("main", 7): Self.data("main 7")], in: sandbox.paths)
        #expect(try await library.importUnit(same).first?.installedOBBs == [])
        #expect(try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int == inode)
        #expect(!sandbox.exists(same))

        let other = try await library.importUnit(Sets.unit([Self.name("main", 7): Self.data("main 7, again")], in: sandbox.paths))
        #expect(other.map(\.decision) == [.unchanged])
        #expect(other.first?.installedOBBs == [Self.obb(Self.name("main", 7), Self.data("main 7, again"))])
        #expect(other.first?.replacedOBBs == [Self.name("main", 7)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 7)) == Self.data("main 7, again"))
    }

    @Test func anAPKAndItsOBBInOnePickInstallTogether() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["base.apk": Sets.base, Self.name("main", 1): Self.data("main 1")], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.install, .unchanged])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1)])
        #expect(await library.entries.first?.obbs == [Self.obb(Self.name("main", 1), Self.data("main 1"))])
        #expect(!sandbox.exists(unit))
    }

    @Test func aUnitWhoseSetAsksKeepsItsOBBUntilTheAnswer() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox, SplitAPK.make(versionCode: 2))
        let unit = try Sets.unit(["base.apk": Sets.base, Self.name("main", 1): Self.data("main 1"),
                                  "level.obb": Self.data("level")], in: sandbox.paths)

        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.olderVersion])])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 1))))
        #expect(sandbox.exists(unit.appendingPathComponent(Self.name("main", 1))))
        // The set's question comes first, so no OBB asks for an app yet.
        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == [])

        let shown = AskingVersion(package: Self.package, versionCode: 1, warnings: [.olderVersion])
        let answered = try await library.importUnit(unit, replacing: [shown])
        #expect(answered.map(\.decision) == [.update, .unchanged])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1)])
        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == ["level.obb"])
    }

    @Test func aFolderUnderItsNameRefusesThePickedOBB() async throws {
        // c2's case for a pick: the app put a folder under main 2's name, so the file cannot go in; the result says why
        // for App's alert, the installed main 1 stays, and the file goes with its unit.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        _ = try await library.importUnit(Sets.unit([Self.name("main", 1): Self.data("main 1")], in: sandbox.paths))
        try FileManager.default.createDirectory(at: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 2)),
                                                withIntermediateDirectories: false)
        let unit = try Sets.unit([Self.name("main", 2): Self.data("main 2")], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.refusedOBBs == [IgnoredFile(path: Self.name("main", 2),
                                                            reason: "something else is in the app’s expansion file folder under that name")])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main 1"))
        #expect(await library.entries.first?.obbs == [Self.obb(Self.name("main", 1), Self.data("main 1"))])
        #expect(!sandbox.exists(unit))
    }

    @Test func aPickedOBBsProgressCountsIt() async throws {
        // The first half of a unit's progress reads its files by their sizes, OBBs too, so it never runs past half.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["base.apk": Sets.base, Self.name("main", 1): Data(count: 1 << 18)], in: sandbox.paths)
        let values = Mutex<[Double]>([])

        _ = try await library.importUnit(unit) { value in values.withLock { $0.append(value) } }

        let shown = values.withLock { $0 }
        #expect(shown.first == 0 && shown.last == 1)
        #expect(zip(shown, shown.dropFirst()).allSatisfy { $0 <= $1 })
    }

    @Test func aLinkUnderItsNameIsReplacedNeverFollowed() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        _ = try await library.importUnit(Sets.unit([Self.name("main", 1): Self.data("main 1")], in: sandbox.paths))
        let secret = sandbox.root.appendingPathComponent("secret.txt")
        try Data("secret".utf8).write(to: secret)
        try FileManager.default.createSymbolicLink(at: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 2)),
                                                   withDestinationURL: secret)

        let results = try await library.importUnit(Sets.unit([Self.name("main", 2): Self.data("main 2")], in: sandbox.paths))

        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 2), Self.data("main 2"))])
        #expect(try Data(contentsOf: secret) == Data("secret".utf8))
        #expect(try FileManager.default.attributesOfItem(atPath: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 2)).path)[.type]
            as? FileAttributeType == .typeRegular)
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 2)])
    }

    @Test func itsBytesUnderItsNameAreRecordedAndMoveNothing() async throws {
        // The app's folder already holds the same bytes under its name, not recorded: the entry records them, and the
        // file stays as it is.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        let file = Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 5))
        try Self.data("main 5").write(to: file)
        let inode = try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int

        let results = try await library.importUnit(Sets.unit([Self.name("main", 5): Self.data("main 5")], in: sandbox.paths))

        #expect(results.first?.installedOBBs == [])
        #expect(await library.entries.first?.obbs == [Self.obb(Self.name("main", 5), Self.data("main 5"))])
        #expect(try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int == inode)
    }

    // MARK: - Which app

    @Test func anOBBWithoutItsAppWaitsForTheUsersAnswer() async throws {
        // No app of its name's package, or no OBB name at all: the unit waits, and the question lists the apps that are
        // ready or need splits, never an incompatible one or a file's entry.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let ready = try await Self.install(library, sandbox)
        _ = try await library.importUnit(Sets.unit(["v7a.apk": SplitAPK.make(package: "org.example.v7a", abis: ["armeabi-v7a"])],
                                                   in: sandbox.paths))
        _ = try await library.importUnit(Sets.unit(["junk.apk": Data("junk".utf8)], in: sandbox.paths))
        let other = Self.name("main", 1, "com.other.app")
        let unit = try Sets.unit([other: Self.data("other"), "level.obb": Self.data("level")], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.isEmpty)
        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == ["level.obb", other])
        #expect(await library.appsForOBBs() == [ready])
        let another = try #require(try await library.importUnit(Sets.unit(["a.apk": SplitAPK.make(package: "org.example.a")],
                                                                          in: sandbox.paths)).first?.entry)
        let waitingForSplits = try #require(try await library.importUnit(
            Sets.unit(["b.apk": SplitAPK.make(package: "org.example.b", manifestSplitRequired: true)], in: sandbox.paths)).first?.entry)
        #expect(waitingForSplits.state == .needsSplits)
        // By display name, as App's question lists them, one that waits for its splits too.
        #expect(await library.appsForOBBs().map(\.displayName) == [another, waitingForSplits, ready].map(\.displayName).sorted())
        #expect(sandbox.exists(unit.appendingPathComponent("level.obb")))
        #expect(try await library.scanUnits().map(\.lastPathComponent) == [unit.lastPathComponent])
    }

    @Test func anOBBOfAnIncompatibleAppWaitsToo() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox, SplitAPK.make(abis: ["armeabi-v7a"]))
        let unit = try Sets.unit([Self.name("main", 1): Self.data("main 1")], in: sandbox.paths)

        #expect(try await library.importUnit(unit).isEmpty)
        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == [Self.name("main", 1)])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 1))))
    }

    @Test func anAnsweredOBBIsNamedForItsApp() async throws {
        // Decision 5: main.<the installed versionCode>.<package>.obb, or patch.… where the old name starts with "patch".
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox, SplitAPK.make(versionCode: 3))
        let other = Self.name("main", 1, "com.other.app")
        let unit = try Sets.unit([other: Self.data("other"), "patch-level.obb": Self.data("patch")], in: sandbox.paths)
        _ = try await library.importUnit(unit)

        let results = try await library.importUnit(unit, assigning: [other: Self.package, "patch-level.obb": Self.package])

        #expect(results.map(\.decision) == [.unchanged, .unchanged])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 3), Self.name("patch", 3)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 3)) == Self.data("other"))
        #expect(await library.entries.first?.obbs.map(\.fileName).sorted() == [Self.name("main", 3), Self.name("patch", 3)])
        #expect(!sandbox.exists(unit))
    }

    @Test func anAnswerForAnAppThatCannotTakeItIsNoAnswer() async throws {
        // Only an app the question lists can take it; any other answer leaves the file waiting.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox, SplitAPK.make(abis: ["armeabi-v7a"]))
        let unit = try Sets.unit(["level.obb": Self.data("level")], in: sandbox.paths)

        _ = try await library.importUnit(unit, assigning: ["level.obb": Self.package, "other.obb": "com.other.app"])

        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == ["level.obb"])
        #expect(sandbox.exists(unit.appendingPathComponent("level.obb")))
    }

    @Test func dontImportLetsTheOneOBBGoAndTheNextOneAsks() async throws {
        // The question names one file, so "Don’t Import" lets that one go, from inside its unit; the
        // other waits for its own question, and the unit goes with its last file. The original stays where the user has
        // it (ADR-0018).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        let unit = try Sets.unit(["a.obb": Self.data("a"), "b.obb": Self.data("b")], in: sandbox.paths)
        _ = try await library.importUnit(unit)

        await library.discardPickedOBB("a.obb", inUnit: unit)

        #expect(await library.obbsWaitingForAnApp(inUnit: unit) == ["b.obb"])
        await library.discardPickedOBB("../b.obb", inUnit: unit)
        #expect(sandbox.exists(unit.appendingPathComponent("b.obb")))
        let results = try await library.importUnit(unit, assigning: ["b.obb": Self.package])
        #expect(results.first?.installedOBBs.map(\.fileName) == [Self.name("main", 1)])
        #expect(!sandbox.exists(unit))
        // A unit goes with its last file let go, and a folder outside incoming/ is none of the library's.
        let last = try Sets.unit(["c.obb": Self.data("c")], in: sandbox.paths)
        await library.discardPickedOBB("c.obb", inUnit: last)
        #expect(!sandbox.exists(last))
        let outside = try sandbox.file(Self.data("d"), named: "d.obb")
        await library.discardPickedOBB("d.obb", inUnit: outside.deletingLastPathComponent())
        #expect(sandbox.exists(outside))
        #expect(try await library.scanUnits().isEmpty)
    }

    @Test func anAppWhoseVersionGivesNoOBBNameIsNoChoice() async throws {
        // An int versionCode below zero names no OBB, so the question never offers that app, which could
        // not take the file; one by its own name still goes there.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox, SplitAPK.make(versionCode: -1))
        #expect(await library.entries.first?.state == .ready)

        #expect(await library.appsForOBBs() == [])
        let results = try await library.importUnit(Sets.unit([Self.name("main", 1): Self.data("main 1")], in: sandbox.paths))
        #expect(results.first?.installedOBBs.map(\.fileName) == [Self.name("main", 1)])
    }

    // MARK: - A big expansion file

    /// The streamed SHA-256 of the file at `url`, in 16 MiB parts, as a hex string.
    static func streamedDigest(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        while let part = try handle.read(upToCount: 16 << 20), !part.isEmpty { hasher.update(data: part) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    @Test func aSparseOBBOfMoreThanTwoGiBIsCheckedAndMovedWithoutSpaceForAllItsBytes() async throws {
        // Games keep their data in one expansion file of 1 to 4 GiB. The unit's file is sparse (a few KiB on disk), so
        // the test costs no space; it checks that the size, the digest and the move survive offsets beyond 2^31 and
        // that the app's copy does not need the room of its length.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox)
        let size: UInt64 = (1 << 31) + (128 << 20)
        let name = Self.name("main", 5)
        let unit = try Sets.unit([:], in: sandbox.paths)
        let source = unit.appendingPathComponent(name)
        let handle = try FileHandle(forWritingTo: {
            FileManager.default.createFile(atPath: source.path, contents: nil)
            return source
        }())
        try handle.truncate(atOffset: size)
        try handle.seek(toOffset: (1 << 31) - 4)
        try handle.write(contentsOf: Data("SPANNING".utf8))
        try handle.close()
        let digest = try Self.streamedDigest(source)

        let results = try await library.importUnit(unit)

        let installed = Self.obbFolder(sandbox.paths).appendingPathComponent(name)
        let attributes = try FileManager.default.attributesOfItem(atPath: installed.path)
        #expect((attributes[.size] as? NSNumber)?.uint64Value == size)
        let expected = PackageFile(fileName: name, splitName: nil, size: size, sha256: digest)
        #expect(results.first?.installedOBBs == [expected])
        #expect(try Self.streamedDigest(installed) == digest)
        var status = stat()
        #expect(stat(installed.path, &status) == 0)
        // Blocks of 512 bytes: far below the length, as the source's were.
        #expect(UInt64(status.st_blocks) * 512 < size / 8)
        #expect(!sandbox.exists(unit))
        let probe = try FileHandle(forReadingFrom: installed)
        defer { try? probe.close() }
        try probe.seek(toOffset: (1 << 31) - 4)
        #expect(try probe.read(upToCount: 8) == Data("SPANNING".utf8))
    }
}
