import Foundation

/// The Debug check's events on the app's console. With MINATO_CHECK_CONSOLE=1 in the launch environment (`devicectl
/// device process launch --console --environment-variables`, `simctl launch --console`) each event also goes to the
/// sink, stderr in the app, as the events file's line behind "minato-check-event ", so the device check reads it from
/// the console and needs no copy through the phone's file service. The line is the one the events file gets, after
/// CheckEventPrivacy's redaction. The guest's log and stdout, which the check's guest-log and guest-out files get, go
/// there too, a line at a time behind "minato-guest-log " and "minato-guest-out " (Lines).
public struct CheckConsole: Sendable {
    public static let variable = "MINATO_CHECK_CONSOLE"
    public static let eventPrefix = "minato-check-event "
    /// The guest's log (its stderr, the check's guest-log file) and its stdout (guest-out) on the console.
    public static let guestLogPrefix = "minato-guest-log "
    public static let guestOutPrefix = "minato-guest-out "

    public let isOn: Bool
    private let sink: @Sendable (String) -> Void

    public init(environment: [String: String], sink: @escaping @Sendable (String) -> Void) {
        isOn = environment[Self.variable] == "1"
        self.sink = sink
    }

    /// An event's line as the events file holds it: the time in seconds with ms, the process, the text on one line.
    public static func line(time: TimeInterval, pid: Int32, text: String) -> String {
        let text = text.replacingOccurrences(of: "\r\n", with: " ").replacingOccurrences(of: "\n", with: " ")
            .replacingOccurrences(of: "\r", with: " ")
        return String(format: "%.3f", time) + " \(pid) \(text)\n"
    }

    /// `line` (from `line(time:pid:text:)`) to the console, when the variable is set.
    public func event(_ line: String) {
        guard isOn else { return }
        sink(Self.eventPrefix + line)
    }

    /// A stream of the guest's bytes cut into lines for the console, behind `prefix`.
    public struct Lines: Sendable {
        let prefix: String
        var pending: [UInt8] = []

        public init(prefix: String) { self.prefix = prefix }
    }

    /// `bytes` of the guest's stream: each line they complete goes to the console, when the variable is set.
    public func guest(_ lines: inout Lines, _ bytes: some Collection<UInt8>) {
        guard isOn else { return }
        lines.pending.append(contentsOf: bytes)
        while let newline = lines.pending.firstIndex(of: UInt8(ascii: "\n")) {
            sink(lines.prefix + String(decoding: lines.pending[..<newline], as: UTF8.self) + "\n")
            lines.pending.removeSubrange(...newline)
        }
    }

    /// The stream ended: what is left goes as one more line.
    public func end(_ lines: inout Lines) {
        guard isOn, !lines.pending.isEmpty else { return }
        sink(lines.prefix + String(decoding: lines.pending, as: UTF8.self) + "\n")
        lines.pending.removeAll()
    }
}
