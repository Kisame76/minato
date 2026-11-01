/// The focused field's visible rect, in the session view's points.
public struct FieldRect: Sendable, Equatable {
    public var left: Double
    public var top: Double
    public var right: Double
    public var bottom: Double

    public init(left: Double, top: Double, right: Double, bottom: Double) {
        self.left = left
        self.top = top
        self.right = right
        self.bottom = bottom
    }

    func contains(_ x: Double, _ y: Double) -> Bool {
        x >= left && x < right && y >= top && y < bottom
    }
}

/// A tap outside the focused field hides the keyboard (ADR-0068 a): the user's wish, and a named departure, since
/// Android never hides it so. Only a tap counts: one finger, while the keyboard is up, that ends within the slop of
/// where it began; a drag or a scroll keeps the keyboard, as iOS apps do while a chat scrolls. Outside a field the
/// guest placed it hides at once, on the field nothing happens. Without a field (an engine that draws its own) the
/// tap may be on the engine's field, which asks to show the keyboard again: so the hide waits for the end of the
/// touch and the grace, and a show from the guest in between cancels it. The touch reaches the guest either way.
public struct OutsideTap: Sendable {
    /// How far a tap may move, in points, as UIKit's tap recognizer allows.
    public static let slop = 10.0
    /// How long a hide without a field waits for the guest's show, in seconds.
    public static let grace = 0.15

    public enum Decision: Sendable, Equatable {
        case none
        case hide
        /// Hide when `graceEnded` says so for this token.
        case hideAfterGrace(UInt64)
    }

    /// Where the focused field lies; nil while the guest has placed none.
    public var field: FieldRect?
    private var touch: UInt?
    private var startX = 0.0
    private var startY = 0.0
    /// The touch is no tap any more: it moved past the slop, or another finger came.
    private var spoiled = false
    private var shown = false
    private var pending: UInt64?
    private var nextToken: UInt64 = 0

    public init() {}

    /// A touch callback of the session's view, in its points.
    public mutating func touched(_ phase: TouchPhase, _ changes: [TouchChange], keyboardShown: Bool) -> Decision {
        switch phase {
        case .began:
            if touch != nil {
                spoiled = true
            } else if keyboardShown, changes.count == 1, let sample = changes[0].samples.last {
                touch = changes[0].touch
                (startX, startY) = (sample.x, sample.y)
                spoiled = false
                shown = false
                pending = nil
            }
            return .none
        case .moved:
            if let mine = changes.first(where: { $0.touch == touch }), !within(mine) { spoiled = true }
            return .none
        case .cancelled:
            if changes.contains(where: { $0.touch == touch }) { touch = nil }
            return .none
        case .ended:
            guard let mine = changes.first(where: { $0.touch == touch }) else { return .none }
            touch = nil
            guard !spoiled, within(mine), !shown else { return .none }
            if let field { return field.contains(startX, startY) ? .none : .hide }
            nextToken += 1
            pending = nextToken
            return .hideAfterGrace(nextToken)
        }
    }

    /// The guest asked to show the keyboard: a tap that may have been on its field hides nothing.
    public mutating func showRequested() {
        if touch != nil { shown = true }
        pending = nil
    }

    /// The grace of `token` is over: whether to hide now.
    public mutating func graceEnded(_ token: UInt64) -> Bool {
        guard pending == token else { return false }
        pending = nil
        return true
    }

    private func within(_ change: TouchChange) -> Bool {
        guard let sample = change.samples.last else { return true }
        let (dx, dy) = (sample.x - startX, sample.y - startY)
        return dx * dx + dy * dy <= Self.slop * Self.slop
    }
}
