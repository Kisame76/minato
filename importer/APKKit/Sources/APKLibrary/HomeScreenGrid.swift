/// How Home and Library lay out their apps (the navigation design, sections 2 and 3): as the iOS Home Screen does on
/// the device, in 4 columns on every iPhone. The icon and the rows are SpringBoard's, measured in the Simulator; the
/// columns spread over the width with margins as wide as the gaps between the icons, as SpringBoard's are within a
/// point or two.
public struct HomeScreenGrid: Equatable, Sendable {
    public static let columns = 4

    /// SpringBoard's icon size per device, one for every screen of it.
    public let iconSize: Double
    /// From the top of an icon to the top of the one below it.
    public let rowPitch: Double
    /// Between the icons and at both sides, for the width the grid has.
    public let spacing: Double

    /// SpringBoard's Home Screen by the screen's width in portrait, in points: icon size and row pitch.
    static let measured: [(screenWidth: Double, iconSize: Double, rowPitch: Double)] = [
        (402, 64, 100.33),  // iPhone 18 Pro
        (430, 68, 106.67),  // iPhone 15 Pro Max
        (440, 68, 108.67),  // iPhone 18 Pro Max
        (466, 60, 98.67),  // iPhone Duo folded
        (669, 60, 98.67),  // iPhone Duo open, with the folded screen's icon
    ]

    /// The grid for a container `width` wide on a screen `screenWidth` wide in portrait; a screen not measured takes
    /// the nearest measured one.
    public init(width: Double, screenWidth: Double) {
        let nearest = Self.measured.min { abs($0.screenWidth - screenWidth) < abs($1.screenWidth - screenWidth) }!
        iconSize = nearest.iconSize
        rowPitch = nearest.rowPitch
        spacing = max(0, (width - Double(Self.columns) * iconSize) / Double(Self.columns + 1))
    }

    /// The space between two rows of tiles whose label line is `labelLineHeight` high, so that the icons keep the
    /// Home Screen's row pitch: a tile has 6 pt around its icon for the selection and its label 8 pt below the icon.
    public func rowSpacing(labelLineHeight: Double) -> Double {
        max(0, rowPitch - iconSize - 14 - labelLineHeight)
    }
}
