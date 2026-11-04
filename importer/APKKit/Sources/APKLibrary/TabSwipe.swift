/// Swiping between the library's tabs (the user's wish, the navigation design): the pages lie side by side and follow
/// the finger, without a loop, so the first and the last tab stop the swipe.
public struct TabSwipe: Sendable, Equatable {
    public let count: Int

    public init(count: Int) {
        self.count = count
    }

    /// The tab a swipe from `index` goes to: the next one for a swipe to the left (a negative translation), the one
    /// before for a swipe to the right; nil at the edges.
    public func target(from index: Int, translation: Double) -> Int? {
        guard translation != 0 else { return nil }
        let target = translation < 0 ? index + 1 : index - 1
        return (0..<count).contains(target) ? target : nil
    }

    /// Whether a move starts a swipe: clearly more across than up or down, so that scrolling wins otherwise.
    public static func isHorizontal(dx: Double, dy: Double) -> Bool {
        abs(dx) > 0 && abs(dx) > abs(dy) * 1.5
    }

    /// How far the swipe has gone towards the next page, 0 to 1.
    public static func progress(translation: Double, width: Double) -> Double {
        guard width > 0 else { return 0 }
        return min(abs(translation) / width, 1)
    }

    /// Points a second that count as a flick.
    static let flick = 500.0

    /// Whether a released swipe goes on to the next page: past half the way, or flicked on towards it; a flick back
    /// springs it back.
    public static func finishes(progress: Double, velocity: Double, translation: Double) -> Bool {
        let towards = velocity * translation > 0
        if abs(velocity) >= flick { return towards }
        return progress > 0.5
    }
}
