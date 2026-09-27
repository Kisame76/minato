import Darwin
import Foundation
import Testing

@testable import APKSession

/// ADR-0032, decision 8: an end that goes wrong is shown at the library's next start, once.
@Suite("LastSession")
struct LastSessionTests {
    let folder: URL
    let store: LastSessionStore

    init() throws {
        folder = FileManager.default.temporaryDirectory.appending(path: "last-session-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        store = LastSessionStore(folder: folder)
    }

    static let session = LastSession(package: "org.example.app", name: "Example")

    /// What the runtime writes to the report descriptor as the program ends (apkrt_report_exit_status).
    static func report(_ status: Int, to descriptor: Int32) {
        let text = "\(status)\n"
        #expect(text.withCString { write(descriptor, $0, strlen($0)) } == text.utf8.count)
        close(descriptor)
    }

    func files() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    @Test func withoutASessionThereIsNothing() {
        #expect(store.take() == nil)
    }

    @Test func aSignalIsShownWithItsNumber() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 11, to: descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .signal(11)))
    }

    @Test func aNormalEndShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(0, to: descriptor)

        #expect(store.take() == nil)
    }

    /// Android shows nothing when an app ends itself, whatever its exit code.
    @Test func anExitCodeShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(3, to: descriptor)

        #expect(store.take() == nil)
    }

    /// iOS ended the process, or Quit restarted it before a program ran: no status.
    @Test func noStatusShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        close(descriptor)

        #expect(store.take() == nil)
    }

    @Test func aStartThatFailedIsShownWithItsReasonEvenWithAStatus() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        store.failed("libmain.so is missing")
        Self.report(1, to: descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .failure("libmain.so is missing")))
    }

    @Test func itIsReadOnceThenRemoved() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 6, to: descriptor)

        #expect(store.take() != nil)
        #expect(store.take() == nil)
        #expect(files() == [])
    }

    /// A new session starts over: the last one's status and reason are gone.
    @Test func aNewSessionForgetsTheLastOne() throws {
        let first = store.begin(Self.session)
        try #require(first >= 0)
        store.failed("reason")
        Self.report(128 + 11, to: first)
        let second = store.begin(LastSession(package: "org.example.other", name: "Other"))
        try #require(second >= 0)
        Self.report(128 + 7, to: second)

        #expect(store.take() == LastSessionNotice(name: "Other", cause: .signal(7)))
    }

    @Test func aDamagedRecordShowsNothingAndIsRemoved() throws {
        try Data("{".utf8).write(to: folder.appending(path: "last-session.json"))
        try Data("139\n".utf8).write(to: folder.appending(path: "last-session.status"))

        #expect(store.take() == nil)
        #expect(files() == [])
    }

    /// Linux's numbers, which the runtime reports: SIGBUS is 7 there, not Darwin's 10.
    @Test func signalsHaveLinuxNames() {
        #expect(LastSessionNotice.signalName(11) == "SIGSEGV")
        #expect(LastSessionNotice.signalName(6) == "SIGABRT")
        #expect(LastSessionNotice.signalName(7) == "SIGBUS")
        #expect(LastSessionNotice.signalName(31) == "SIGSYS")
        #expect(LastSessionNotice.signalName(40) == nil)
    }
}
