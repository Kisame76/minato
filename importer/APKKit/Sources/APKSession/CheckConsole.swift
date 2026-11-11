import Foundation

/// The Debug check's events on the app's console. With MINATO_CHECK_CONSOLE=1 in the launch environment (`devicectl
/// device process launch --console --environment-variables`, `simctl launch --console`) each event also goes to the
/// sink, stderr in the app, as the events file's line behind "minato-check-event ", so the device check reads it from
/// the console and needs no copy through the phone's file service. The line is the one the events file gets, after
/// CheckEventPrivacy's redaction, and nothing else goes there.
public struct CheckConsole: Sendable {
    public static let variable = "MINATO_CHECK_CONSOLE"
    public static let eventPrefix = "minato-check-event "

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
}
