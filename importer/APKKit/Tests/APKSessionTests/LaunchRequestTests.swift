import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary
@testable import APKSession

/// ADR-0032, decision 1 and test 1: the request that makes the next launch game mode, read once.
@Suite("Launch request")
struct LaunchRequestTests {
    /// A folder of its own, as Application Support.
    struct Folder {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("apksession-\(UUID().uuidString)")

        init() throws {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        }

        func remove() { try? FileManager.default.removeItem(at: url) }
    }

    /// An entry as the library holds it, analysed from SampleAPK.
    static func entry(package: String = "org.example.game", state: LibraryEntry.State = .ready,
                      nativeActivity: Bool = true, signedWith: String? = "aa") throws -> LibraryEntry {
        let report = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(package: package, nativeActivity: nativeActivity)),
                                             preferredLanguages: ["en"])
        var entry = LibraryEntry(
            id: package, displayName: report.label, state: state, report: report,
            base: PackageFile(fileName: "base.apk", splitName: nil, size: 1, sha256: "cc"), splits: [], obbs: [],
            source: SourceFile(fileName: "game.apk", size: 1, modifiedMilliseconds: 0), iconFile: nil, importedAt: Date())
        entry.signedWith = signedWith
        entry.folder = "0123456789abcdef"
        return entry
    }

    @Test func isWrittenAndReadOnce() throws {
        let folder = try Folder()
        defer { folder.remove() }
        let store = LaunchRequestStore(folder: folder.url)
        let request = LaunchRequest(package: "org.example.game", time: Date(timeIntervalSince1970: 1_790_000_000))

        try store.write(request)

        #expect(store.take() == request)
        // Read once: a crash in game mode never leads into game mode again.
        #expect(store.take() == nil)
        #expect(!FileManager.default.fileExists(atPath: store.file.path))
    }

    @Test func keepsTheTimeToTheMillisecond() throws {
        // Game mode logs the time from the tap to its process, which takes about half a second.
        let folder = try Folder()
        defer { folder.remove() }
        let store = LaunchRequestStore(folder: folder.url)
        let time = Date(timeIntervalSince1970: 1_790_385_412.876)

        try store.write(LaunchRequest(package: "org.example.game", time: time))

        let taken = try #require(store.take())
        #expect(abs(taken.time.timeIntervalSince(time)) < 0.001)
    }

    @Test func isWrittenAtomically() throws {
        // A new request replaces the old one whole, through a temporary file.
        let folder = try Folder()
        defer { folder.remove() }
        let store = LaunchRequestStore(folder: folder.url)
        try store.write(LaunchRequest(package: "org.example.first", time: Date(timeIntervalSince1970: 1)))

        try store.write(LaunchRequest(package: "org.example.second", time: Date(timeIntervalSince1970: 2)))

        #expect(try FileManager.default.contentsOfDirectory(atPath: folder.url.path) == ["launch-request.json"])
        #expect(store.take()?.package == "org.example.second")
    }

    @Test func anUnreadableRequestIsDeletedAndGivesNone() throws {
        let folder = try Folder()
        defer { folder.remove() }
        let store = LaunchRequestStore(folder: folder.url)
        try Data("not json".utf8).write(to: store.file)

        #expect(store.take() == nil)
        #expect(!FileManager.default.fileExists(atPath: store.file.path))
    }

    @Test func withoutARequestTheLibraryStarts() throws {
        #expect(LaunchMode(request: nil, entries: [try Self.entry()]) == .library(nil))
    }

    @Test func aRequestForAReadyNativeActivityAppStartsGameMode() throws {
        let entry = try Self.entry()

        #expect(LaunchMode(request: LaunchRequest(package: entry.id, time: Date()), entries: [entry]) == .gameMode(entry))
    }

    @Test func aRequestForAnAppNoLongerThereStartsTheLibraryWithAMessage() throws {
        let mode = LaunchMode(request: LaunchRequest(package: "org.example.gone", time: Date()), entries: [try Self.entry()])

        #expect(mode == .library(.appNotFound("org.example.gone")))
    }

    @Test(arguments: [LibraryEntry.State.needsSplits, .incompatible, .invalid("broken")])
    func aRequestForAnAppThatIsNotReadyStartsTheLibraryWithAMessage(state: LibraryEntry.State) throws {
        let entry = try Self.entry(state: state)

        #expect(LaunchMode(request: LaunchRequest(package: entry.id, time: Date()), entries: [entry]) == .library(.appNotReady(entry)))
    }

    /// ADR-0039 c5: a ready app with Java code starts on ART, as a NativeActivity app starts.
    @Test func aRequestForAReadyAppWithJavaCodeStartsGameMode() throws {
        let entry = try Self.entry(nativeActivity: false)

        #expect(LaunchMode(request: LaunchRequest(package: entry.id, time: Date()), entries: [entry]) == .gameMode(entry))
    }
}
