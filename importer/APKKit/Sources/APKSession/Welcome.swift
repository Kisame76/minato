import Foundation

/// Whether the library shows its welcome sheet, which says how to add and start apps and how to get back to Minato
/// from one: once at Minato's first launch, and once after an update for whoever never saw it. It counts as seen once
/// it is dismissed; Settings shows it again.
public struct Welcome {
    public static let key = "welcomeShown"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public var needsShowing: Bool { !defaults.bool(forKey: Self.key) }

    public func shown() {
        defaults.set(true, forKey: Self.key)
    }
}
