/// The interface orientations Minato allows, as UIKit's mask without UIKit (ADR-0038, decision 1).
public struct InterfaceOrientations: OptionSet, Sendable {
    public let rawValue: Int

    public init(rawValue: Int) {
        self.rawValue = rawValue
    }

    public static let portrait = InterfaceOrientations(rawValue: 1 << 0)
    public static let landscapeLeft = InterfaceOrientations(rawValue: 1 << 1)
    public static let landscapeRight = InterfaceOrientations(rawValue: 1 << 2)
    public static let landscape: InterfaceOrientations = [.landscapeLeft, .landscapeRight]
}

extension LaunchMode {
    /// The library stays in portrait (decision 1.1), and so does compile mode's screen. Game mode allows Info.plist's
    /// three until its session holds its own orientation.
    public var orientations: InterfaceOrientations {
        switch self {
        case .library, .compileMode: .portrait
        case .gameMode: [.portrait, .landscape]
        }
    }
}
