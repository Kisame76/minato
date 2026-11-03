import Testing

@testable import APKLibrary

/// The navigation design, sections 2 and 3: Home and Library lay out their apps as the iOS Home Screen does, with
/// SpringBoard's icon and rows measured in the Simulator (points at scale 3), in 4 columns on every iPhone.
@Suite("Home Screen grid")
struct HomeScreenGridTests {
    /// SpringBoard's icon and row pitch (icon top to icon top) per device, by the screen's width in portrait.
    @Test(arguments: [
        (402.0, 64.0, 100.33),  // iPhone 18 Pro: 192 px icons, rows 301 px apart
        (430.0, 68.0, 106.67),  // iPhone 15 Pro Max: 204 px, 320 px
        (440.0, 68.0, 108.67),  // iPhone 18 Pro Max: 204 px, 326 px
        (466.0, 60.0, 98.67),  // iPhone Duo folded: 180 px, 296 px
        (669.0, 60.0, 98.67),  // iPhone Duo open: one icon per device, the folded one's
    ])
    func theIconAndTheRowsAreTheHomeScreens(screenWidth: Double, icon: Double, rowPitch: Double) {
        let grid = HomeScreenGrid(width: screenWidth, screenWidth: screenWidth)

        #expect(grid.iconSize == icon)
        #expect(abs(grid.rowPitch - rowPitch) < 0.01)
    }

    @Test func aScreenNotMeasuredTakesTheNearestMeasuredOne() {
        // The iPhone 17, 16 Plus and Air sit between the measured widths.
        #expect(HomeScreenGrid(width: 393, screenWidth: 393).iconSize == 64)
        #expect(HomeScreenGrid(width: 420, screenWidth: 420).iconSize == 68)
    }

    /// The margins as wide as the gaps between the icons, as SpringBoard's are within a point or two: the Pro Max's 35
    /// and 32.7, the 15 Pro Max's 33 and 30.7, the 18 Pro's 30.3 and 28.6.
    @Test(arguments: [(402.0, 29.2), (430.0, 31.6), (440.0, 33.6), (466.0, 45.2), (669.0, 85.8)])
    func fourColumnsWithTheMarginsAsWideAsTheGaps(width: Double, spacing: Double) {
        let grid = HomeScreenGrid(width: width, screenWidth: width)

        #expect(abs(grid.spacing - spacing) < 0.01)
        #expect(abs(4 * grid.iconSize + 5 * grid.spacing - width) < 0.01)
    }

    @Test func theRowSpacingLeavesTheTilesRowPitch() {
        // A tile: 6 pt around the icon for the selection, its label 8 pt below the icon in one line.
        let grid = HomeScreenGrid(width: 440, screenWidth: 440)

        #expect(abs(grid.rowSpacing(labelLineHeight: 14.32) - (108.67 - 68 - 14 - 14.32)) < 0.01)
    }

    @Test func aNarrowWidthKeepsTheIconAndNoNegativeSpace() {
        let grid = HomeScreenGrid(width: 200, screenWidth: 440)

        #expect(grid.iconSize == 68)
        #expect(grid.spacing == 0)
        #expect(grid.rowSpacing(labelLineHeight: 200) == 0)
    }
}
