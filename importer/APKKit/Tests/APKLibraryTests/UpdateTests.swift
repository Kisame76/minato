import APKKit
import APKSigning
import APKTestSupport
import CryptoKit
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0034 step a): each version in a folder of its own, the switch through the index, what an update
/// keeps, and the files that change nothing.
@Suite("Library updates")
struct UpdateTests {
    typealias Sandbox = LibraryTests.Sandbox

    struct Interrupted: Error {}

    /// What lies in apps/<package>/, sorted.
    static func appFolder(_ sandbox: Sandbox, _ package: String = "org.example.game") -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: sandbox.paths.installDirectory(for: package).path)) ?? []).sorted()
    }

    /// The first 16 hex digits of the SHA-256, which name a version's folder.
    static func folder(of data: Data) -> String {
        String(SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined().prefix(16))
    }

    static func installedAPK(_ sandbox: Sandbox, _ entry: LibraryEntry) throws -> Data {
        try Data(contentsOf: sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk"))
    }

    // MARK: - Installing and updating

    @Test func installsIntoAFolderNamedAfterTheFile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(sandbox.file(apk, named: "game.apk"))

        #expect(result.decision == .install)
        #expect(result.entry.folder == Self.folder(of: apk))
        #expect(Self.appFolder(sandbox) == [Self.folder(of: apk)])
        #expect(try Self.installedAPK(sandbox, result.entry) == apk)
        #expect(result.entry.iconFile == "apps/org.example.game/\(Self.folder(of: apk))/icon.png")
        #expect(result.entry.updatedAt == nil)
        #expect(result.report?.versionCode == 1)
    }

    @Test func anUpdateKeepsDataPlaceAndImportDateAndRemovesTheOldFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        _ = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.other"), named: "other.apk"))
        let save = sandbox.paths.dataDirectory(for: first.id).appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)
        let before = Date()

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.report?.versionCode == 2)
        #expect(result.entry.importedAt == first.importedAt)
        let updatedAt = try #require(result.entry.updatedAt)
        #expect(updatedAt >= before)
        #expect(await library.entries.map(\.id) == ["org.example.game", "org.example.other"])
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
        let folder = Self.folder(of: SampleAPK.make(versionCode: 2))
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, result.entry) == SampleAPK.make(versionCode: 2))
        // What belongs to the version is the new one's.
        #expect(result.entry.base?.sha256.hasPrefix(folder) == true)
        #expect(result.entry.source.fileName == "v2.apk")
        #expect(result.entry.iconFile == "apps/org.example.game/\(folder)/icon.png")
        // After a restart the index names the new version.
        #expect(try await Library(paths: sandbox.paths).entries.first?.folder == result.entry.folder)
    }

    @Test func anUpdateStartsWithItsCodeUnsigned() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: first)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: lib.appendingPathComponent("libmain.so"))
        try await library.resign(id: first.id, fingerprint: "aa", with: ResigningTests.RecordingConverter(fingerprint: "aa"))

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        // The old version's code went with its folder; the new version's is not converted or signed yet.
        #expect(result.entry.signedWith == nil)
        #expect(!sandbox.exists(lib))
        #expect(await library.resignCandidates() == [ResignCandidate(id: first.id, signedWith: nil, containerCount: 0)])
    }

    @Test func anUpdateTakesTheNewVersionsState() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1, splitRequired: true), named: "v1.apk")).entry
        #expect(first.state == .needsSplits)

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.state == .ready)
    }

    @Test func anUpdateKeepsWhatItDoesNotReplace() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let obb = PackageFile(fileName: "main.1.org.example.game.obb", splitName: nil, size: 3, sha256: "ee")
        let split = PackageFile(fileName: "split_config.xxhdpi.apk", splitName: "config.xxhdpi", size: 4, sha256: "ff")
        try await library.record(id: first.id) {
            $0.obbs = [obb]
            $0.splits = [split]
        }

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        // OBBs belong to the entry, split APKs to the version (ADR-0034, decision 6).
        #expect(result.entry.id == first.id)
        #expect(result.entry.obbs == [obb])
        #expect(result.entry.splits.isEmpty)
    }

    @Test func theSameVersionWithOtherBytesInstallsAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(label: "Before"), named: "a.apk")).entry
        let before = Date()

        let result = try await library.importFile(sandbox.file(SampleAPK.make(label: "After"), named: "b.apk"))

        #expect(result.decision == .reinstall)
        #expect(result.entry.displayName == "After")
        #expect(result.entry.importedAt == first.importedAt)
        let updatedAt = try #require(result.entry.updatedAt)
        #expect(updatedAt >= before)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(label: "After"))])
    }

    @Test func anOlderVersionStillReplacesTheInstalledOneUntilStepBAsks() async throws {
        // The policy asks; the question and the waiting file come with step b).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))

        #expect(result.decision == .ask([.olderVersion]))
        #expect(result.entry.report?.versionCode == 1)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 1))])
    }

    // MARK: - Files that change nothing

    @Test(arguments: [(["armeabi-v7a"], false, LibraryEntry.State.incompatible), (["arm64-v8a"], true, .needsSplits)])
    func anUnusableFileLeavesTheWorkingVersion(abis: [String], splitRequired: Bool, state: LibraryEntry.State) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(
            sandbox.file(SampleAPK.make(versionCode: 2, abis: abis, splitRequired: splitRequired), named: "v2.apk"))
        let recorder = ProgressRecorder()

        let result = try await library.importFile(picked) { recorder.add($0) }

        let folder = try #require(installed.folder)
        #expect(recorder.values.last == 1)
        #expect(result.decision == .reject(state))
        #expect(result.report?.versionCode == 2)
        #expect(result.entry == installed)
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))
        #expect(!sandbox.exists(picked))
    }

    @Test(arguments: [["arm64-v8a"], ["armeabi-v7a"]])
    func theSameBytesChangeNothing(abis: [String]) async throws {
        // Also for an incompatible version, which keeps no base.apk but the file's SHA-256.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(abis: abis)
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(apk, named: "game.apk")).entry
        let picked = try await library.adopt(sandbox.file(apk, named: "again.apk"))
        let recorder = ProgressRecorder()

        let result = try await library.importFile(picked) { recorder.add($0) }

        let folder = try #require(installed.folder)
        #expect(recorder.values.last == 1)
        #expect(result.decision == .unchanged)
        #expect(result.entry == installed)
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(!sandbox.exists(picked))
    }

    // MARK: - Interruptions

    @Test func anInterruptedUpdateLeavesTheOldVersionAndIsFinishedLater() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }

        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }

        // The new folder is complete, but the index still names the old one.
        let folder = try #require(installed.folder)
        let relaunched = try Library(paths: sandbox.paths)
        #expect(await relaunched.entries.first?.folder == folder)
        #expect(Self.appFolder(sandbox) == [folder, Self.folder(of: SampleAPK.make(versionCode: 2))].sorted())
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))

        let waiting = try await relaunched.scan()

        #expect(Self.appFolder(sandbox) == [folder])
        #expect(LibraryTests.resolved(waiting) == LibraryTests.resolved([picked]))
        let result = try await relaunched.importFile(try #require(waiting.first))
        #expect(result.decision == .update)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
    }

    @Test func theSameFileAfterAnInterruptedImportBuildsItsFolderAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }
        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }
        await library.interruptBeforeTheSwitch {}

        // No scan in between: the complete folder of the interrupted import is still there.
        let result = try await library.importFile(picked)

        #expect(result.decision == .update)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
        #expect(try Self.installedAPK(sandbox, result.entry) == SampleAPK.make(versionCode: 2))
    }

    @Test func aVersionFolderInUseIsNeverCleared() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(apk, named: "game.apk")).entry
        let folder = try #require(installed.folder)
        // As if another file's SHA-256 began like the installed one's: the same folder, other bytes.
        try await library.record(id: installed.id) {
            $0.source.sha256 = String(repeating: "0", count: 64)
            $0.base?.sha256 = String(repeating: "0", count: 64)
        }
        let picked = try await library.adopt(sandbox.file(apk, named: "again.apk"))

        await #expect(throws: LibraryError.versionFolderInUse(folder)) { try await library.importFile(picked) }

        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == apk)
    }

    @Test func aRecordThatCannotBeWrittenChangesNothing() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: installed)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: lib.appendingPathComponent("libmain.so"))
        try await library.resign(id: installed.id, fingerprint: "aa", with: ResigningTests.RecordingConverter(fingerprint: "aa"))
        // A folder stands where the index would be written.
        try FileManager.default.removeItem(at: sandbox.paths.indexFile)
        try FileManager.default.createDirectory(at: sandbox.paths.indexFile, withIntermediateDirectories: true)

        await #expect(throws: (any Error).self) {
            try await library.resign(id: installed.id, fingerprint: "bb", with: ResigningTests.RecordingConverter(fingerprint: "bb"))
        }
        await #expect(throws: (any Error).self) { try await library.forgetSignatures() }
        await #expect(throws: (any Error).self) { try await library.remove(id: installed.id) }

        // The library still holds what the index on disk says.
        #expect(await library.entries.map(\.id) == [installed.id])
        #expect(await library.entries.first?.signedWith == "aa")
    }

    @Test(arguments: [["arm64-v8a"], ["armeabi-v7a"]])
    func anIndexRewrittenByAnOlderBuildKeepsTheVersion(abis: [String]) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(abis: abis)
        let installed = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry
        let folder = try #require(installed.folder)
        // An older Minato build decodes the index and writes it again without the fields it does not know.
        let json = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: sandbox.paths.indexFile)) as? [String: Any])
        let entries = try #require(json["entries"] as? [[String: Any]])
        var old = json
        old["entries"] = entries.map { entry in
            var older = entry
            older["folder"] = nil
            older["updatedAt"] = nil
            if var source = older["source"] as? [String: Any] {
                source["sha256"] = nil
                older["source"] = source
            }
            // With a base.apk only its SHA-256 leads to the folder, without one only the icon's path.
            if older["base"] != nil { older["iconFile"] = nil }
            return older
        }
        try JSONSerialization.data(withJSONObject: old).write(to: sandbox.paths.indexFile)
        let written = try String(decoding: Data(contentsOf: sandbox.paths.indexFile), as: UTF8.self)
        #expect(!written.contains("folder"))

        // The folder is found again, by the base.apk's SHA-256 or else by the icon's path, so the
        // launch's scan keeps it.
        let library = try Library(paths: sandbox.paths)
        _ = try await library.scan()

        let entry = try #require(await library.entries.first)
        #expect(entry.folder == folder)
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(sandbox.exists(sandbox.paths.versionDirectory(for: entry).appendingPathComponent("icon.png")))
    }

    @Test func anInterruptedFirstInstallLeavesNothingBehind() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(), named: "game.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }
        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make())])

        let waiting = try await Library(paths: sandbox.paths).scan()

        #expect(!sandbox.exists(sandbox.paths.installDirectory(for: "org.example.game")))
        #expect(LibraryTests.resolved(waiting) == LibraryTests.resolved([picked]))
    }

    @Test func aSwitchThatCannotBeWrittenLeavesTheOldVersion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        // A folder stands where the index would be written, so the switch fails.
        try FileManager.default.removeItem(at: sandbox.paths.indexFile)
        try FileManager.default.createDirectory(at: sandbox.paths.indexFile, withIntermediateDirectories: true)

        await #expect(throws: (any Error).self) { try await library.importFile(picked) }

        // The library still names the old version, so a scan removes the new folder, not the old one.
        let folder = try #require(installed.folder)
        #expect(await library.entries == [installed])
        _ = try await library.scan()
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))
    }

    // MARK: - Entries from before version folders

    /// The index as the library wrote it before version folders: the entries without `folder` and `updatedAt`.
    struct OldIndex: Encodable {
        var version = 1
        var entries: [LibraryEntry]
    }

    @Test func anEntryFromBeforeVersionFoldersUpdatesIntoAFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // The layout before ADR-0034: base.apk, the icon and lib/ right in apps/<package>/.
        let imported = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let app = sandbox.paths.installDirectory(for: imported.id)
        let version = sandbox.paths.versionDirectory(for: imported)
        for name in try FileManager.default.contentsOfDirectory(atPath: version.path) {
            try FileManager.default.moveItem(at: version.appendingPathComponent(name), to: app.appendingPathComponent(name))
        }
        try FileManager.default.removeItem(at: version)
        try FileManager.default.createDirectory(at: app.appendingPathComponent("lib"), withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: app.appendingPathComponent("lib/libold.so"))
        var old = imported
        old.folder = nil
        old.iconFile = "apps/\(imported.id)/icon.png"
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let index = try encoder.encode(OldIndex(entries: [old]))
        #expect(!String(decoding: index, as: UTF8.self).contains("folder"))
        try index.write(to: sandbox.paths.indexFile)
        let save = sandbox.paths.dataDirectory(for: imported.id).appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)

        // Next to them, what an interrupted update left: a version folder, and names that are none.
        for name in ["0123456789abcdef", "notaversionfolde", "abc"] {
            try FileManager.default.createDirectory(at: app.appendingPathComponent(name), withIntermediateDirectories: true)
        }

        let library = try Library(paths: sandbox.paths)
        let entry = try #require(await library.entries.first)
        #expect(entry.folder == nil)
        #expect(sandbox.paths.versionDirectory(for: entry) == app)
        // A launch keeps the old layout's files, which belong to the entry, and removes only the version folder.
        #expect(try await library.scan().isEmpty)
        #expect(Self.appFolder(sandbox) == ["abc", "base.apk", "icon.png", "lib", "notaversionfolde"])
        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: nil, containerCount: 1)])

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.importedAt == entry.importedAt)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
    }
}
