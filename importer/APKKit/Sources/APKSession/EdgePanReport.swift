/// Whether a pan is game mode's swipe down from the top edge (ADR-0032, decision 9), and the Debug log's lines
/// about it: where each pan near the top starts, whether it is the menu's swipe, and whether it reached the
/// menu. A swipe the user felt without a line was taken by iOS before the view saw it. Points throughout; the
/// view spans the screen, so its top is the screen's in every orientation.
public enum EdgePanReport {
    public enum Verdict: Sendable, Equatable {
        /// The view lets the pan begin. iOS may still take its touches: then no end line follows.
        case accepted
        /// The pan starts `topEdge` or more below the top.
        case refusedStart
        /// The pan goes sideways or up more than down.
        case refusedDirection

        var text: String {
            switch self {
            case .accepted: "accepted"
            case .refusedStart: "refused: start"
            case .refusedDirection: "refused: direction"
            }
        }
    }

    public struct Start: Sendable, Equatable {
        public let verdict: Verdict
        public let line: String
    }

    /// Whether a pan's start lies in the band the log covers, 3 × `topEdge` from the top: the misses just below
    /// the edge count, the guest's ordinary touches further down do not.
    public static func isNearTop(startY: Double, topEdge: Double) -> Bool {
        startY < 3 * topEdge
    }

    /// The menu's swipe starts above `topEdge` and goes down more than sideways.
    public static func verdict(startY: Double, velocityX: Double, velocityY: Double, topEdge: Double) -> Verdict {
        guard startY < topEdge else { return .refusedStart }
        return velocityY > abs(velocityX) ? .accepted : .refusedDirection
    }

    /// The pan's verdict with its line, in a view of `width` × `height`; nil outside the band.
    public static func start(x: Double, y: Double, width: Double, height: Double, velocityX: Double,
                             velocityY: Double, topEdge: Double) -> Start? {
        guard isNearTop(startY: y, topEdge: topEdge) else { return nil }
        let verdict = verdict(startY: y, velocityX: velocityX, velocityY: velocityY, topEdge: topEdge)
        let line = "edge pan \(whole(x)) \(whole(y)) in \(whole(width))x\(whole(height)) "
            + "velocity \(whole(velocityX)) \(whole(velocityY)) \(verdict.text)"
        return Start(verdict: verdict, line: line)
    }

    /// The end of a pan that began: how far it went down, and whether the menu opened.
    public static func end(distance: Double, menu: Bool) -> String {
        "edge pan end \(whole(distance)) menu \(menu ? "yes" : "no")"
    }

    private static func whole(_ value: Double) -> Int {
        Int(value.rounded())
    }
}
