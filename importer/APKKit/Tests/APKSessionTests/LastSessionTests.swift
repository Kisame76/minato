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

    /// The runtime's report with a record after the status line, as it writes it at a kill of itself or a crash.
    static func report(_ text: String, to descriptor: Int32) {
        #expect(text.withCString { write(descriptor, $0, strlen($0)) } == text.utf8.count)
        close(descriptor)
    }

    func files() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    @Test func withoutASessionThereIsNothing() {
        #expect(store.take() == nil)
    }

    @Test func clearingAnAppsDataForgetsItsLastSessionOnly() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 9, to: descriptor)
        store.forget(package: "org.example.other")
        #expect(files() == ["last-session.json", "last-session.status"])
        store.forget(package: Self.session.package)
        #expect(files().isEmpty)
    }

    @Test func aSignalIsShownWithItsNumber() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 11, to: descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .signal(11), package: Self.session.package))
    }

    @Test func aNormalEndShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(0, to: descriptor)

        #expect(store.take() == nil)
    }

    /// Android shows nothing when an app ends itself with an exit code; Minato does, since the app is gone without a
    /// word otherwise (the decision, 2026-11-01), also after the end handler ran.
    @Test func anExitCodeIsShown() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(3, to: descriptor)
        #expect(store.take() == LastSessionNotice(
            name: "Example", cause: .exitStatus(3), package: Self.session.package))

        let again = store.begin(Self.session)
        try #require(again >= 0)
        Self.report(1, to: again)
        store.ended()
        #expect(store.take() == LastSessionNotice(
            name: "Example", cause: .exitStatus(1), package: Self.session.package))
    }

    /// iOS ended the process, as for using too much memory: no status and no end, since a kill by iOS runs nothing.
    @Test func noStatusAndNoEndIsAnEndByIOS() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        close(descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .endedByIOS, package: Self.session.package))
    }

    /// Quit restarted Minato before a program ran: no status, but the end was the user's.
    @Test func aQuitBeforeTheProgramRanShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        close(descriptor)
        store.ended()

        #expect(store.take() == nil)
    }

    @Test func aStartThatFailedIsShownWithItsReasonEvenWithAStatus() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        store.failed("libmain.so is missing")
        Self.report(1, to: descriptor)

        #expect(store.take() == LastSessionNotice(
            name: "Example", cause: .failure("libmain.so is missing"), package: Self.session.package))
    }

    /// A program that ends through the end handler ends as an exit, a quit or a kill of itself, whose report a
    /// self-kill makes 137 (Runtime's ADR-0028 decision 5, as Godot ends its process): no crash, so nothing is shown.
    @Test func anEndThroughTheEndHandlerShowsNothingEvenAfterAKill() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 9, to: descriptor)
        store.ended()

        #expect(store.take() == nil)
    }

    /// The same report without the end handler, as when a kill from outside ends the process: shown as a crash.
    @Test func aKillWithoutTheEndHandlerIsShownWithItsSignal() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report(128 + 9, to: descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .signal(9), package: Self.session.package))
    }

    /// A kill of itself without the end handler, with the runtime's record after the status: the status line is the
    /// signal, the record's lines are kept for the report (a status read over the whole file was no number, and the
    /// kill read as an end by iOS).
    @Test func aKillOfItselfKeepsTheRuntimesRecord() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report("137\nkilled itself with signal 9 (Killed)\n#0 libc.so+0x68f0c\n#1 libc.so+0x5b164\n",
                    to: descriptor)

        #expect(store.take() == LastSessionNotice(
            name: "Example", cause: .signal(9), package: Self.session.package,
            record: ["killed itself with signal 9 (Killed)", "#0 libc.so+0x68f0c", "#1 libc.so+0x5b164"]))
    }

    /// The same report through the end handler is no crash: nothing is shown, as before the record.
    @Test func aRecordThroughTheEndHandlerShowsNothing() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        Self.report("137\nkilled itself with signal 9 (Killed)\n#0 libc.so+0x68f0c\n", to: descriptor)
        store.ended()

        #expect(store.take() == nil)
    }

    /// A crash's record: signal, pc, lr and frames. Blank lines go, and at most 12 lines of at most 160 characters
    /// are kept, so that a broken file cannot fill the alert's report.
    @Test func aCrashsRecordIsKeptWithinItsBounds() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        let frames = (0..<20).map { "#\($0) app library 1+0x\(String($0, radix: 16))" }
        let long = "pc: " + String(repeating: "f", count: 300)
        Self.report((["139", "signal: 11 (SIGSEGV)", "", long] + frames).joined(separator: "\n") + "\n", to: descriptor)

        let notice = try #require(store.take())
        #expect(notice.cause == .signal(11))
        try #require(notice.record.count == 12)
        #expect(notice.record.first == "signal: 11 (SIGSEGV)")
        #expect(notice.record[1] == String(long.prefix(160)))
        #expect(notice.record.last == "#9 app library 1+0x9")
    }

    /// A record of an older build, without the end, reads as before: a crash is still shown.
    @Test func anOlderRecordWithoutTheEndStillShowsACrash() throws {
        let descriptor = store.begin(Self.session)
        try #require(descriptor >= 0)
        try Data(#"{"package":"org.example.app","name":"Example"}"#.utf8).write(to: store.recordFile)
        Self.report(128 + 11, to: descriptor)

        #expect(store.take() == LastSessionNotice(name: "Example", cause: .signal(11), package: Self.session.package))
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

        #expect(store.take() == LastSessionNotice(name: "Other", cause: .signal(7), package: "org.example.other"))
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
