/// The ways Minato restarts itself (ADR-0032, decision 2); their names are tools/session-check.sh's.
public enum RestartWay: String, CaseIterable, Sendable {
    /// minato://open, opened twice with UIApplication.open (public API). A second install of Minato, such as
    /// SideStore's beside Xcode's, answers it too.
    case ownURL = "url"
    /// LSApplicationWorkspace with this install's bundle ID (private API).
    case workspace
    /// Neither: the alert asks the user to open Minato again.
    case none
}

/// The order a restart tries its ways in (ADR-0032, decision 2, as it was refined on 2026-10-01): the
/// workspace first, since it reaches this install only, then the URL, then the fallback. A requested way is the
/// only one tried, so session-check can force each.
public struct RestartSequence: Sendable, Equatable {
    public enum Step: Sendable, Equatable {
        /// Try this way, with its own timeout.
        case attempt(RestartWay)
        /// A way was answered: the process ends.
        case exit
        /// Every way failed: the alert asks the user to open Minato again.
        case fallback
    }

    public static let defaultOrder: [RestartWay] = [.workspace, .ownURL]

    private let ways: [RestartWay]
    private var index: Int?
    private var settled = false

    public init(requested: RestartWay?) {
        ways = requested.map { [$0] } ?? Self.defaultOrder
    }

    /// The first way; nil once the sequence has started.
    public mutating func start() -> Step? {
        guard index == nil else { return nil }
        index = 0
        return .attempt(ways[0])
    }

    /// `way` opened Minato again. nil when it is not the way being tried.
    public mutating func answered(_ way: RestartWay) -> Step? {
        guard isCurrent(way) else { return nil }
        settled = true
        return .exit
    }

    /// `way` was refused, could not run, or went unanswered: the next way, or the fallback after the last. nil
    /// when it is not the way being tried.
    public mutating func failed(_ way: RestartWay) -> Step? {
        guard isCurrent(way), let index else { return nil }
        guard index + 1 < ways.count else {
            settled = true
            return .fallback
        }
        self.index = index + 1
        return .attempt(ways[index + 1])
    }

    private func isCurrent(_ way: RestartWay) -> Bool {
        guard !settled, let index else { return false }
        return ways[index] == way
    }
}
