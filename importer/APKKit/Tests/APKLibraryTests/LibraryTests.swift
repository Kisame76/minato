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
        let installed = sandbox.paths.installDirectory(for: "org.example.game").appendingPathComponent("base.apk")
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

        let entry = try await library.importFile(file)

        #expect(entry.id == "org.example.game")
        #expect(entry.displayName == "My Game")
        #expect(entry.state == .ready)
        #expect(entry.report?.versionCode == 7)
        let installed = sandbox.paths.installDirectory(for: "org.example.game").appendingPathComponent("base.apk")
        #expect(try Data(contentsOf: installed) == apk)
        #expect(entry.base?.size == UInt64(apk.count))
        #expect(entry.base?.sha256.count == 64)
        let icon = try #require(entry.iconFile.map { sandbox.paths.applicationSupport.appendingPathComponent($0) })
        #expect(try Data(contentsOf: icon).prefix(8) == SampleAPK.pngSignature)
        for folder in ["data", "external", "obb"] {
            #expect(sandbox.exists(sandbox.paths.dataDirectory(for: "org.example.game").appendingPathComponent(folder)))
        }
        #expect(await library.entries.map(\.id) == ["org.example.game"])
    }

    @Test func importOfIncompatibleAPKKeepsReportButNoCopy() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(SampleAPK.make(package: "org.example.old", abis: ["armeabi-v7a"]), named: "old.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file)

        #expect(entry.state == .incompatible)
        #expect(entry.report?.compatibility.issues.contains(.only32Bit(["armeabi-v7a"])) == true)
        #expect(entry.base == nil)
        #expect(!sandbox.exists(sandbox.paths.installDirectory(for: "org.example.old").appendingPathComponent("base.apk")))
    }

    @Test func importOfBaseNeedingSplitsKeepsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(SampleAPK.make(package: "org.example.split", splitRequired: true), named: "base.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file)

        #expect(entry.state == .needsSplits)
        #expect(entry.base != nil)
    }

    @Test func importOfBrokenFileRecordsWhy() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let file = try sandbox.file(Data("this is not an apk".utf8), named: "broken.apk")
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(file)

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

        let updated = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(updated.report?.versionCode == 2)
        #expect(await library.entries.count == 1)
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
        let installed = sandbox.paths.installDirectory(for: "org.example.game").appendingPathComponent("base.apk")
        #expect(try Data(contentsOf: installed) == SampleAPK.make(versionCode: 2))
    }

    @Test func rejectsPackageNamesThatEscapeTheLibrary() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let entry = try await library.importFile(sandbox.file(SampleAPK.make(package: "../../escape"), named: "evil.apk"))

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
