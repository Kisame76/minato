import Foundation
import Testing

@testable import APKSession

/// An app that closed itself soon after it opened: Get Info says so in its footer until the app's next start, since
/// neither Android nor iOS shows a word, and the library coming back looks like a crash.
@Suite("ClosedItself")
struct ClosedItselfTests {
    let folder: URL
    let store: ClosedItselfStore

    init() throws {
        folder = FileManager.default.temporaryDirectory.appending(path: "closed-itself-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        store = ClosedItselfStore(folder: folder)
    }

    func seconds(_ end: SessionEnd, byUser: Bool = false, after: Double) -> Int? {
        ClosedItselfStore.seconds(after: end, byUser: byUser, seconds: after)
    }

    /// An exit with 0, a kill of itself (137) and the app's own finish count, rounded, at least 1 s.
    @Test func theAppsOwnEndsWithinTenSecondsCount() {
        #expect(seconds(.exited(0), after: 2.6) == 3)
        #expect(seconds(.exited(128 + 9), after: 4) == 4)
        #expect(seconds(.lifecycleEnded, after: 10) == 10)
        #expect(seconds(.exited(0), after: 0.2) == 1)
    }

    @Test func laterEndsDoNotCount() {
        #expect(seconds(.exited(0), after: 10.01) == nil)
        #expect(seconds(.lifecycleEnded, after: 600) == nil)
    }

    /// The menu's Quit is the user's end, even at once.
    @Test func theUsersQuitDoesNotCount() {
        #expect(seconds(.lifecycleEnded, byUser: true, after: 1) == nil)
        #expect(seconds(.exited(0), byUser: true, after: 1) == nil)
    }

    /// An exit code from 1 to 128 has the library's notice, a failed start its reason, a broken channel is no end of
    /// the app's.
    @Test func endsWithTheirOwnNoticeDoNotCount() {
        #expect(seconds(.exited(1), after: 1) == nil)
        #expect(seconds(.exited(128), after: 1) == nil)
        #expect(seconds(.startFailed, after: 1) == nil)
        #expect(seconds(.channelBroke, after: 1) == nil)
    }

    @Test func aRecordIsKeptPerPackage() {
        #expect(store.seconds(of: "org.example.app") == nil)
        store.record(package: "org.example.app", seconds: 3)
        store.record(package: "org.example.other", seconds: 7)
        #expect(store.seconds(of: "org.example.app") == 3)
        #expect(store.seconds(of: "org.example.other") == 7)
        // Read as often as Get Info opens.
        #expect(store.seconds(of: "org.example.app") == 3)
    }

    /// Until the app's next start, which forgets its record only; clearing its data or deleting it forgets it too.
    @Test func theNextStartForgetsTheAppsRecord() {
        store.record(package: "org.example.app", seconds: 3)
        store.record(package: "org.example.other", seconds: 7)
        store.forget(package: "org.example.app")
        #expect(store.seconds(of: "org.example.app") == nil)
        #expect(store.seconds(of: "org.example.other") == 7)
        store.forget(package: "org.example.other")
        #expect(((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).isEmpty)
    }

    @Test func aDamagedFileIsNoRecord() throws {
        try Data("{".utf8).write(to: store.file)
        #expect(store.seconds(of: "org.example.app") == nil)
        store.record(package: "org.example.app", seconds: 2)
        #expect(store.seconds(of: "org.example.app") == 2)
    }
}
