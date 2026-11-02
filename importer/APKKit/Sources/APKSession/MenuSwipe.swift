/// Game mode's menu swipe (ADR-0032 decision 9, amended 2026-11-01): a swipe down with three fingers anywhere opens
/// the menu, once per swipe, as the top-edge swipe does. Points throughout.
public struct MenuSwipe: Sendable {
    /// The fingers the swipe takes, at least.
    public static let fingers = 3
    /// How far down it goes before the menu opens.
    public static let distance = 60.0

    private var opened = false

    public init() {}

    /// Whether a pan of `touches` fingers is the menu's: three or more, down more than sideways.
    public static func begins(touches: Int, velocityX: Double, velocityY: Double) -> Bool {
        touches >= fingers && velocityY > abs(velocityX)
    }

    /// A new swipe: it may open the menu again.
    public mutating func began() {
        opened = false
    }

    /// Whether the swipe, now `distance` points down, opens the menu: the first time it is past `Self.distance`.
    public mutating func moved(distance: Double) -> Bool {
        guard !opened, distance > Self.distance else { return false }
        opened = true
        return true
    }
}
