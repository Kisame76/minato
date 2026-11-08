import Darwin
import Foundation

/// The app of game mode's session, as the library shows it after an end that went wrong (ADR-0032,
/// decision 8).
public struct LastSession: Codable, Equatable, Sendable {
    public var package: String
    public var name: String
    /// Why the app could not start (START_FAILED, or `apkrt_start_program`'s error), if it could not.
    public var failure: String?
    /// Whether the program ended through the end handler: an exit, a quit or a kill of itself, never a crash; nil in
    /// a record of an older build.
    public var endedItself: Bool?

    public init(package: String, name: String, failure: String? = nil) {
        self.package = package
        self.name = name
        self.failure = failure
    }
}

/// What the library shows at its next start: "“<name>” quit unexpectedly." with the signal or the reason.
public struct LastSessionNotice: Equatable, Sendable {
    public enum Cause: Equatable, Sendable {
        /// Linux's signal number.
        case signal(Int32)
        case failure(String)
        /// No status and no end: iOS ended the process, which runs nothing, as for using too much memory.
        case endedByIOS
        /// The app ended itself with exit(N), N from 1 to 128: Minato's own notice, since Android shows nothing.
        case exitStatus(Int32)
    }

    public var name: String
    public var cause: Cause
    /// The app's package, for its report (ADR-0070); nil in a notice made without one.
    public var package: String?
    /// The runtime's record after the status line, as it writes it at a kill of itself or a crash: the cause, then
    /// the registers and frames with the libraries named or masked (Runtime); empty in a status of an older build.
    public var record: [String]

    public init(name: String, cause: Cause, package: String? = nil, record: [String] = []) {
        self.name = name
        self.cause = cause
        self.package = package
        self.record = record
    }

    private static let signalNames = [
        "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE", "SIGKILL", "SIGUSR1",
        "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT", "SIGCHLD", "SIGCONT", "SIGSTOP",
        "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGURG", "SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH",
        "SIGIO", "SIGPWR", "SIGSYS",
    ]

    /// The name of Linux's signal `number`, nil for a real-time one.
    public static func signalName(_ number: Int32) -> String? {
        signalNames.indices.contains(Int(number) - 1) ? signalNames[Int(number) - 1] : nil
    }
}

/// The last session's two files in Library/Application Support: `last-session.json`, which game mode writes
/// at its start and when the app cannot start, and `last-session.status`, to which the runtime writes the
/// exit status as the program ends (`apkrt_report_exit_status`), also at a crash, which ends the process
/// without the end handler. So nothing is left to do at the end on a guest's small stack.
public struct LastSessionStore: Sendable {
    public let folder: URL

    public init(folder: URL) {
        self.folder = folder
    }

    var recordFile: URL { folder.appending(path: "last-session.json") }
    var statusFile: URL { folder.appending(path: "last-session.status") }

    /// At game mode's start, before the program: the record, and the status file emptied and opened for the
    /// runtime's report. Returns its descriptor, which stays open for the process's life, or -1.
    public func begin(_ session: LastSession) -> Int32 {
        let descriptor = open(statusFile.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o600)
        guard descriptor >= 0 else { return -1 }
        write(session)
        return descriptor
    }

    /// The program ended through the end handler (ADR-0032, decision 7): an exit, a quit or a kill of itself, which
    /// the runtime reports as 137 and ends as an exit (ADR-0028, decision 5); or game mode restarts to the library.
    /// A crash ends the process without the handler, and iOS's kill runs nothing, so their records keep no end.
    public func ended() {
        guard var session = read() else { return }
        session.endedItself = true
        write(session)
    }

    /// The app could not start, for `reason`.
    public func failed(_ reason: String) {
        guard var session = read() else { return }
        session.failure = reason
        write(session)
    }

    /// The app `package`'s data was cleared: its last session is no more to report.
    public func forget(package: String) {
        guard read()?.package == package else { return }
        try? FileManager.default.removeItem(at: recordFile)
        try? FileManager.default.removeItem(at: statusFile)
    }

    /// Read once, then removed: what the library shows, nil for an end that went right.
    public func take() -> LastSessionNotice? {
        let session = read()
        let (status, record) = Self.status((try? String(contentsOf: statusFile, encoding: .utf8)) ?? "")
        try? FileManager.default.removeItem(at: recordFile)
        try? FileManager.default.removeItem(at: statusFile)
        guard let session else { return nil }
        let notice = { LastSessionNotice(name: session.name, cause: $0, package: session.package, record: record) }
        if let failure = session.failure { return notice(.failure(failure)) }
        // An exit code, through the end handler or not (the decision, 2026-11-01).
        if let status, (1...128).contains(status) {
            return notice(.exitStatus(status))
        }
        // Otherwise an end through the end handler is no crash, a kill of itself's 137 too; nor is a restart to the
        // library, which marks the end too.
        if session.endedItself == true { return nil }
        // Neither the runtime's status nor an end: iOS ended the process (a kill by iOS runs nothing).
        guard let status else { return notice(.endedByIOS) }
        // A shell's 128 + the signal.
        guard status > 128 else { return nil }
        return notice(.signal(status - 128))
    }

    /// Lines of the record kept, and characters of each: a cause, pc, lr and 8 frames, with room to spare.
    static let recordLines = 12
    static let recordLineLength = 160

    /// The status file's first line, the exit status or 128 + the signal, as the number; the lines after it, which
    /// the runtime writes at a kill of itself or a crash, as the record, blank ones left out.
    static func status(_ text: String) -> (Int32?, [String]) {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        guard let first = lines.first else { return (nil, []) }
        let record = lines.dropFirst()
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
            .prefix(recordLines)
            .map { String($0.prefix(recordLineLength)) }
        return (Int32(first.trimmingCharacters(in: .whitespacesAndNewlines)), Array(record))
    }

    private func read() -> LastSession? {
        (try? Data(contentsOf: recordFile)).flatMap { try? JSONDecoder().decode(LastSession.self, from: $0) }
    }

    private func write(_ session: LastSession) {
        try? JSONEncoder().encode(session).write(to: recordFile, options: .atomic)
    }
}
