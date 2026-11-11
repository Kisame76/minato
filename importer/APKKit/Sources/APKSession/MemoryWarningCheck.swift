import Foundation

/// The Debug request "warning" of java-start-check (SessionCheck): it posts iOS's memory warning, as many times as its
/// file says, through the path iOS's own warning takes (GameModeView's handler, the session, the bridge's onLowMemory
/// rate limit), and each gives the event "memory warning delivered: <note>".
public enum MemoryWarningCheck {
    /// The most warnings one request posts.
    public static let most = 10

    /// The warnings the request's file asks for: its number from 1 to `most`, 1 for an empty, missing or unreadable
    /// one.
    public static func count(of text: String?) -> Int {
        guard let number = text.flatMap({ Int($0.trimmingCharacters(in: .whitespacesAndNewlines)) }), number > 0
        else { return 1 }
        return min(number, most)
    }

    /// What the bridge did with a warning: "passed on" (an onLowMemory went to the guest), "held back, <n> s since the
    /// last" (the rate limit), or "held back, not started or waiting" (the lifecycle gives none before the start and
    /// holds events while it waits for an answer).
    public static func note(passedOn: Bool, since: Duration?) -> String {
        if passedOn { return "passed on" }
        guard let since else { return "held back, not started or waiting" }
        return "held back, \(since.components.seconds) s since the last"
    }
}
