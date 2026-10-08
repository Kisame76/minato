import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

@Suite("Library")
struct LibraryTests {
    /// A fresh app container (Documents + Application Support) in a temporary directory, next to a
    /// Downloads folder that stands for the user's own files outside the app.
    struct Sandbox {
        let root: URL
        let paths: LibraryPaths
        let downloads: URL

        init() throws {
            root = FileManager.default.temporaryDirectory.appendingPathComponent("apklibrary-\(UUID().uuidString)")
            paths = LibraryPaths(
                documents: root.appendingPathComponent("Documents"),
                applicationSupport: root.appendingPathComponent("Library/Application Support"))
            downloads = root.appendingPathComponent("Downloads")
            try FileManager.default.createDirectory(at: paths.documents, withIntermediateDirectories: true)
        }

        /// A file the user could pick with +, in Downloads unless another folder is given.
        func file(_ data: Data, named name: String, in folder: URL? = nil) throws -> URL {
            let folder = folder ?? downloads
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let url = folder.appendingPathComponent(name)
            try data.write(to: url)
            return url
        }

        func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

        func remove() { try? FileManager.default.removeItem(at: root) }
    }

    /// The temporary directory is reached through the /var -> /private/var symlink, so compare resolved paths.
    static func resolved(_ urls: [URL]) -> [String] {
        urls.map { $0.resolvingSymlinksInPath().path }
    }

    // MARK: - Picking files

    @Test func pickedFilesWaitHiddenUntilInstalled() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let original = try sandbox.file(SampleAPK.make(), named: "game.apk")
        let library = try Library(paths: sandbox.paths)

        let adopted = try await library.adopt(original)

        #expect(sandbox.exists(original)) // the original stays where it was
        #expect(try Data(contentsOf: adopted) == SampleAPK.make())
        // Nothing lands in Documents, the folder the Files app would show.
        #expect(try FileManager.default.contentsOfDirectory(atPath: sandbox.paths.documents.path).isEmpty)
        #expect(Self.resolved(try await library.scan()) == Self.resolved([adopted]))
    }

    @Test func pickingTwoFilesWithTheSameNameKeepsBoth() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let fromDownloads = try sandbox.file(SampleAPK.make(package: "org.example.one"), named: "game.apk")
        let fromICloud = try sandbox.file(SampleAPK.make(package: "org.example.two"), named: "game.apk", in: sandbox.root.appendingPathComponent("iCloud"))
        let library = try Library(paths: sandbox.paths)

        let first = try await library.adopt(fromDownloads)
        let second = try await library.adopt(fromICloud)

        #expect(try Data(contentsOf: first) == SampleAPK.make(package: "org.example.one"))
        #expect(try Data(contentsOf: second) == SampleAPK.make(package: "org.example.two"))
        #expect(Set(Self.resolved(try await library.scan())) == Set(Self.resolved([first, second])))
    }

    @Test func pickedFilesLeaveNoCopyBehind() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let game = try sandbox.file(SampleAPK.make(), named: "game.apk")
        let junk = try sandbox.file(Data("junk".utf8), named: "junk.apk")
        let library = try Library(paths: sandbox.paths)

        for original in [game, junk] {
            let adopted = try await library.adopt(original)
            _ = try await library.importFile(adopted)
            #expect(!sandbox.exists(adopted))
            #expect(sandbox.exists(original))
        }

        #expect(await library.entries.count == 2)
        #expect(try await library.scan().isEmpty)
        let entry = try #require(await library.entries.first(where: { $0.id == "org.example.game" }))
        let installed = sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")
        #expect(try Data(contentsOf: installed) == SampleAPK.make())
    }

    @Test func anInterruptedImportIsFinishedLater() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let adopted = try await Library(paths: sandbox.paths).adopt(sandbox.file(SampleAPK.make(), named: "game.apk"))

        // The app was closed before the import ran; the next launch finds the file again.
        let relaunched = try Library(paths: sandbox.paths)

        #expect(Self.resolved(try await relaunched.scan()) == Self.resolved([adopted]))
    }

    @Test func pickingADeletedAppAgainReinstallsIt() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let original = try sandbox.file(SampleAPK.make(), named: "game.apk")
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(library.adopt(original))
        try await library.remove(id: "org.example.game")

        let adopted = try await library.adopt(original)

        #expect(Self.resolved(try await library.scan()) == Self.resolved([adopted]))
    }

    @Test func copiesThatIOSPutInTheInboxAreCleanedUp() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // "Open with" from Mail or Safari hands over a copy in Documents/Inbox.
        let handedOver = try sandbox.file(SampleAPK.make(), named: "game.apk", in: sandbox.paths.inbox)
        let library = try Library(paths: sandbox.paths)

        let adopted = try await library.adopt(handedOver)

        #expect(!sandbox.exists(handedOver))
        #expect(try Data(contentsOf: adopted) == SampleAPK.make())
    }

    // MARK: - Installing

    @Test func importInstallsReadyAPK() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(package: "org.example.game", versionCode: 7, label: "My Game")
        let file = try sandbox.file(apk, named: "game.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file).entry

        #expect(entry.id == "org.example.game")
        #expect(entry.displayName == "My Game")
        #expect(entry.state == .ready)
        #expect(entry.report?.versionCode == 7)
        let installed = sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")
        #expect(try Data(contentsOf: installed) == apk)
        #expect(entry.base?.size == UInt64(apk.count))
        #expect(entry.base?.sha256.count == 64)
        let icon = try #require(entry.iconFile.map { sandbox.paths.applicationSupport.appendingPathComponent($0) })
        #expect(try Data(contentsOf: icon).prefix(8) == SampleAPK.pngSignature)
        // installd's createAppData makes cache and code_cache in the app's credential and device protected data at the
        // install (ADR-0039 d6).
        for folder in ["data", "data/cache", "data/code_cache", "data_de", "data_de/cache", "data_de/code_cache",
                       "external", "obb"] {
            #expect(sandbox.exists(sandbox.paths.dataDirectory(for: "org.example.game").appendingPathComponent(folder)))
        }
        #expect(await library.entries.map(\.id) == ["org.example.game"])
    }

    /// An app imported before ADR-0039's decision 6, or before its device protected data, gets installd's folders at
    /// its next start; its files stay.
    @Test func preparingTheDataAddsWhatIsMissingAndKeepsTheRest() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let data = sandbox.paths.dataDirectory(for: "org.example.game")
        let save = data.appendingPathComponent("data/save.dat")
        try FileManager.default.createDirectory(at: save.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data("kept".utf8).write(to: save)

        try sandbox.paths.prepareData(for: "org.example.game")

        for folder in ["data/cache", "data/code_cache", "data_de/cache", "data_de/code_cache", "external", "obb"] {
            #expect(sandbox.exists(data.appendingPathComponent(folder)))
        }
        #expect(try Data(contentsOf: save) == Data("kept".utf8))
    }

    static func permissions(_ url: URL) throws -> Int {
        try #require(FileManager.default.attributesOfItem(atPath: url.path)[.posixPermissions] as? Int)
    }

    /// The version's APKs and signers.json are read-only, as Android's installer leaves an app's APKs to it: the guest
    /// cannot write them (LibraryPaths.makeReadOnly, ADR-0039 c5).
    @Test func theVersionsAPKAndSignersAreReadOnly() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry

        let version = sandbox.paths.versionDirectory(for: entry)
        #expect(try Self.permissions(version.appendingPathComponent("base.apk")) == 0o444)
        #expect(try Self.permissions(version.appendingPathComponent("signers.json")) == 0o444)
    }

    /// A Java app's guest /data lies in Caches (the lead's decision for ADR-0039 c5).
    @Test func theGuestsDataIsInCaches() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }

        #expect(sandbox.paths.caches.path == sandbox.root.appendingPathComponent("Library/Caches").path)
        #expect(sandbox.paths.guestDataDirectory(for: "org.example.game").path
            == sandbox.root.appendingPathComponent("Library/Caches/guest/org.example.game/data").path)
    }

    /// At every start of a Java app, since iOS may empty Caches: the guest's /data with ART's dalvik-cache, user/0,
    /// user_de/0, data and the code path's lib/arm64, as framework/tests/app_process_run.py makes them, and no local;
    /// and the version's files read-only, which an app imported before ADR-0039 c5 gets then.
    @Test func preparingAJavaStartMakesTheGuestsDataAndTheFilesReadOnly() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let version = sandbox.paths.versionDirectory(for: entry)
        for name in ["base.apk", "signers.json"] {
            try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: version.appendingPathComponent(name).path)
        }

        for _ in 0..<2 {
            let data = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

            #expect(data.path == sandbox.paths.guestDataDirectory(for: entry.id).path)
            #expect(sandbox.exists(data.appendingPathComponent("dalvik-cache/arm64")))
            #expect(sandbox.exists(data.appendingPathComponent("user/0")))
            // The parent of the app's device protected data mount, root's 0751 too.
            #expect(sandbox.exists(data.appendingPathComponent("user_de/0")))
            // /data/data, the parent of the app's data mount, which the guest stats as Android's root 0751 (ADR-0052's
            // owners); /data/local stays absent, as the owner model counts /data/local/tmp as the app's.
            #expect(sandbox.exists(data.appendingPathComponent("data")))
            #expect(!sandbox.exists(data.appendingPathComponent("local")))
            #expect(sandbox.exists(data.appendingPathComponent("app/~~AAAA/org.example.game-BBBB==/lib/arm64")))
            for name in ["base.apk", "signers.json"] {
                #expect(try Self.permissions(version.appendingPathComponent(name)) == 0o444)
            }
            try FileManager.default.removeItem(at: sandbox.paths.caches)
        }
    }

    /// An unsigned APK has no signers.json, which the start does without.
    @Test func preparingAJavaStartWithoutSignersWorks() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths)
            .importFile(sandbox.file(SampleAPK.make(signers: []), named: "game.apk")).entry

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        #expect(try Self.permissions(sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")) == 0o444)
    }

    /// ART's class loader opens <code path>/lib/arm64/<name> before it loads a JNI library (DexPathList's
    /// findNativeLibrary), and the loader then takes the version's container: the guest's lib/arm64 holds read-only
    /// clones of the version's ELF originals, made again at every start, and no guest write reaches an original that
    /// resign converts and signs (ADR-0039 c5).
    @Test func preparingAJavaStartClonesTheLibrariesWhereARTLooksForThem() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let original = sandbox.paths.libraryDirectory(for: entry).appendingPathComponent("libgame.so")
        let bytes = try Data(contentsOf: original)

        let data = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        let clone = data.appendingPathComponent("app/~~AAAA/org.example.game-BBBB==/lib/arm64/libgame.so")
        #expect(try Data(contentsOf: clone) == bytes)
        #expect(try Self.permissions(clone) == 0o444)
        #expect(try FileManager.default.attributesOfItem(atPath: clone.path)[.type] as? FileAttributeType == .typeRegular)
        // A guest that makes the clone writable and writes it, as the runtime lets it (chmod and open pass through).
        try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: clone.path)
        try Data("written by the guest".utf8).write(to: clone)
        #expect(try Data(contentsOf: original) == bytes)

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        #expect(try Data(contentsOf: clone) == bytes)
        #expect(try Self.permissions(clone) == 0o444)
    }

    /// A library that the version's lib/ no longer holds goes from the guest's lib/arm64, and another code path, an
    /// older version's, goes with its clones, so that Caches keeps no old libraries.
    @Test func preparingAJavaStartKeepsOnlyTheVersionsLibraries() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let apps = sandbox.paths.guestDataDirectory(for: entry.id).appendingPathComponent("app")
        let lib = apps.appendingPathComponent("~~AAAA/org.example.game-BBBB==/lib/arm64")
        for folder in [lib, apps.appendingPathComponent("~~AAAA/org.example.game-CCCC==/lib/arm64"),
                       apps.appendingPathComponent("~~DDDD/org.example.game-EEEE==/lib/arm64")] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data("old".utf8).write(to: folder.appendingPathComponent("libold.so"))
        }

        _ = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        #expect(try FileManager.default.contentsOfDirectory(atPath: lib.path) == ["libgame.so"])
        #expect(try FileManager.default.contentsOfDirectory(atPath: apps.path) == ["~~AAAA"])
        #expect(try FileManager.default.contentsOfDirectory(atPath: apps.appendingPathComponent("~~AAAA").path)
            == ["org.example.game-BBBB=="])
    }

    /// A Java app without JNI libraries gets an empty lib/arm64, as on Android.
    @Test func preparingAJavaStartWithoutLibrariesLeavesLibEmpty() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths)
            .importFile(sandbox.file(SampleAPK.make(abis: []), named: "game.apk")).entry

        let data = try sandbox.paths.prepareJavaStart(for: entry, codePath: "/data/app/~~AAAA/org.example.game-BBBB==")

        #expect(try FileManager.default.contentsOfDirectory(
            atPath: data.appendingPathComponent("app/~~AAAA/org.example.game-BBBB==/lib/arm64").path) == [])
    }

    @Test func importOfIncompatibleAPKKeepsReportButNoCopy() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(SampleAPK.make(package: "org.example.old", abis: ["armeabi-v7a"]), named: "old.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file).entry

        #expect(entry.state == .incompatible)
        #expect(entry.report?.compatibility.issues.contains(.only32Bit(["armeabi-v7a"])) == true)
        #expect(entry.base == nil)
        #expect(!sandbox.exists(sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")))
    }

    @Test func importOfBaseNeedingSplitsKeepsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(SampleAPK.make(package: "org.example.split", splitRequired: true), named: "base.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file).entry

        #expect(entry.state == .needsSplits)
        #expect(entry.base != nil)
    }

    @Test func importOfBrokenFileRecordsWhy() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(Data("this is not an apk".utf8), named: "broken.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file).entry

        #expect(entry.id == "file:broken.apk")
        #expect(entry.displayName == "broken.apk")
        guard case .invalid = entry.state else {
            Issue.record("expected invalid state, got \(entry.state)")
            return
        }
    }

    @Test func updateReplacesInstallAndKeepsAppData() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))
        let save = sandbox.paths.dataDirectory(for: "org.example.game").appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)

        let updated = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk")).entry

        #expect(updated.report?.versionCode == 2)
        #expect(await library.entries.count == 1)
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
        let installed = sandbox.paths.versionDirectory(for: updated).appendingPathComponent("base.apk")
        #expect(try Data(contentsOf: installed) == SampleAPK.make(versionCode: 2))
    }

    @Test func rejectsPackageNamesThatEscapeTheLibrary() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(sandbox.file(SampleAPK.make(package: "../../escape"), named: "evil.apk")).entry

        #expect(entry.id == "file:evil.apk")
        #expect(!sandbox.exists(sandbox.paths.applicationSupport.appendingPathComponent("escape")))
        #expect(!sandbox.exists(sandbox.root.appendingPathComponent("escape")))
        #expect(!sandbox.exists(sandbox.root.appendingPathComponent("Library/escape")))
    }

    @Test func reportsProgressFromStartToEnd() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let recorder = ProgressRecorder()

        _ = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk")) { recorder.add($0) }

        let values = recorder.values
        #expect(values.first.map { $0 < 0.5 } == true)
        #expect(values.last == 1.0)
        #expect(values == values.sorted())
    }

    // MARK: - Index and removal

    @Test func indexSurvivesRestart() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        _ = try await Library(paths: sandbox.paths).importFile(sandbox.file(SampleAPK.make(label: "Persistent"), named: "game.apk"))

        let reopened = try Library(paths: sandbox.paths)

        #expect(await reopened.entries.map(\.displayName) == ["Persistent"])
    }

    @Test func removeDeletesInstallAndData() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk"))

        try await library.remove(id: "org.example.game")

        #expect(await library.entries.isEmpty)
        #expect(!sandbox.exists(sandbox.paths.installDirectory(for: "org.example.game")))
        #expect(!sandbox.exists(sandbox.paths.dataDirectory(for: "org.example.game")))
        #expect(try await Library(paths: sandbox.paths).entries.isEmpty)
    }
}

final class ProgressRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [Double] = []

    func add(_ value: Double) {
        lock.withLock { recorded.append(value) }
    }

    var values: [Double] { lock.withLock { recorded } }
}
