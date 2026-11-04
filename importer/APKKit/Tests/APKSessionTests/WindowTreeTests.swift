import Testing

@testable import APKSession

/// The iOS host's tree of the session's layers (ADR-0041 gap 4, plan step 6 (c)): c4's surfaces and the requested
/// windows as they come and go, in WindowOrder's order split at the session's window, and the dim's amount.
@Suite("WindowTree")
struct WindowTreeTests {
    // WindowManager.LayoutParams' types.
    static let application: Int32 = 2
    static let panel: Int32 = 1000
    static let media: Int32 = 1001
    static let toast: Int32 = 2005

    @Test func anEmptyTreeHasNothingAroundTheSessionsWindow() {
        let tree = WindowTree()
        #expect(tree.split.below.isEmpty)
        #expect(tree.split.above.isEmpty)
        #expect(tree.dimAmount == 0)
    }

    /// A dimmed dialog lies above the window's surfaces, the dim just below it, and its popup above it.
    @Test func aDimmedDialogWithItsPopupLiesAboveTheSurfaces() {
        var tree = WindowTree()
        tree.addSurface(id: 21, z: -2)
        tree.addSurface(id: 22, z: 1)
        tree.addWindow(token: 7, type: Self.application, parent: 0, group: 0, dim: 0.6)
        tree.addWindow(token: 8, type: Self.panel, parent: 7, group: 0, dim: 0)
        #expect(tree.split.below == [.surface(21)])
        #expect(tree.split.above == [.surface(22), .dim, .window(7), .window(8)])
        #expect(tree.dimAmount == 0.6)
    }

    /// A sub-window of the session's window with a negative sublayer lies below the window.
    @Test func aMediaSubWindowOfTheSessionsWindowLiesBelowIt() {
        var tree = WindowTree()
        tree.addWindow(token: 9, type: Self.media, parent: 0, group: 0, dim: 0)
        #expect(tree.split.below == [.window(9)])
        #expect(tree.split.above.isEmpty)
    }

    /// The dim follows the topmost dimming window, and goes with the last one.
    @Test func theDimFollowsTheTopmostDimmingWindow() {
        var tree = WindowTree()
        tree.addWindow(token: 7, type: Self.application, parent: 0, group: 0, dim: 0.6)
        tree.addWindow(token: 8, type: Self.application, parent: 0, group: 0, dim: 0.3)
        #expect(tree.split.above == [.window(7), .dim, .window(8)])
        #expect(tree.dimAmount == 0.3)
        tree.placeWindow(token: 8, dim: 0)
        #expect(tree.split.above == [.dim, .window(7), .window(8)])
        #expect(tree.dimAmount == 0.6)
        tree.remove(.window(7))
        #expect(tree.split.above == [.window(8)])
        #expect(tree.dimAmount == 0)
    }

    /// A toast lies above every application window, made earlier or later.
    @Test func aToastLiesAboveTheApplicationWindows() {
        var tree = WindowTree()
        tree.addWindow(token: 5, type: Self.toast, parent: 0, group: 0, dim: 0)
        tree.addWindow(token: 7, type: Self.application, parent: 0, group: 0, dim: 0)
        #expect(tree.split.above == [.window(7), .window(5)])
    }

    /// A surface's new z moves it to the other side; a place of an unknown one changes nothing.
    @Test func aSurfacesNewZMovesItAcrossTheWindow() {
        var tree = WindowTree()
        tree.addSurface(id: 21, z: 1)
        tree.placeSurface(id: 21, z: -1)
        tree.placeSurface(id: 99, z: 1)
        tree.placeWindow(token: 99, dim: 0.5)
        #expect(tree.split.below == [.surface(21)])
        #expect(tree.split.above.isEmpty)
        #expect(tree.dimAmount == 0)
    }

    /// The make order counts surfaces and windows alike, so a removed window's newer sibling keeps its place.
    @Test func makesAreCountedAcrossSurfacesAndWindows() {
        var tree = WindowTree()
        tree.addWindow(token: 7, type: Self.application, parent: 0, group: 0, dim: 0)
        tree.addSurface(id: 21, z: 1)
        tree.addWindow(token: 8, type: Self.application, parent: 0, group: 0, dim: 0)
        tree.remove(.window(7))
        tree.addWindow(token: 9, type: Self.application, parent: 0, group: 0, dim: 0)
        #expect(tree.split.above == [.surface(21), .window(8), .window(9)])
    }
}
