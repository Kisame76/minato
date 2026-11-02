import Testing

@testable import APKSession

/// ADR-0041 decision 8: the guest's insets from iOS's safe area, per edge, as Android 15 reports them under gesture
/// navigation.
struct GuestInsetsTests {
    /// An iPhone with the Dynamic Island at the top in portrait: 1206 x 2622 px at scale 3.
    let island = GuestInsets.Device(cutout: .top, cutoutLength: 126, scale: 3)

    @Test func inPortraitTheIslandIsTheTopsCutout() {
        let insets = GuestInsets(safeArea: .init(top: 62, left: 0, bottom: 34, right: 0),
                                 size: PixelSize(width: 1206, height: 2622), orientation: .portrait, device: island)
        #expect(insets.cutout == GuestInsets.Cutout(edge: .top, inset: 186,
                                                    rect: PixelRect(left: 414, top: 0, right: 792, bottom: 186)))
        // 30 dp at 480 dpi, and the home indicator's inset.
        #expect(insets.systemGestures == GuestInsets.Edges(left: 90, top: 0, right: 90, bottom: 102))
        #expect(insets.mandatorySystemGestures == GuestInsets.Edges(left: 0, top: 0, right: 0, bottom: 102))
    }

    @Test func turnedTheCutoutFollowsTheIslandsSideNotBoth() {
        let safeArea = GuestInsets.SafeArea(top: 0, left: 62, bottom: 21, right: 62)
        let size = PixelSize(width: 2622, height: 1206)
        // landscapeRight: the device's top is on the screen's left.
        let right = GuestInsets(safeArea: safeArea, size: size, orientation: .landscapeRight, device: island)
        #expect(right.cutout == GuestInsets.Cutout(edge: .left, inset: 186,
                                                   rect: PixelRect(left: 0, top: 414, right: 186, bottom: 792)))
        let left = GuestInsets(safeArea: safeArea, size: size, orientation: .landscapeLeft, device: island)
        #expect(left.cutout == GuestInsets.Cutout(edge: .right, inset: 186,
                                                  rect: PixelRect(left: 2436, top: 414, right: 2622, bottom: 792)))
        #expect(left.systemGestures == GuestInsets.Edges(left: 90, top: 0, right: 90, bottom: 63))
    }

    @Test func aCutoutOnTheRightInPortraitTurnsToTheTop() {
        // The iPhone Duo's sensor bar: the right edge in portrait.
        let duo = GuestInsets.Device(cutout: .right, cutoutLength: 100, scale: 3)
        let portrait = GuestInsets(safeArea: .init(top: 0, left: 0, bottom: 20, right: 40),
                                   size: PixelSize(width: 1398, height: 2034), orientation: .portrait, device: duo)
        #expect(portrait.cutout == GuestInsets.Cutout(edge: .right, inset: 120,
                                                      rect: PixelRect(left: 1278, top: 867, right: 1398,
                                                                      bottom: 1167)))
        let turned = GuestInsets(safeArea: .init(top: 40, left: 0, bottom: 20, right: 0),
                                 size: PixelSize(width: 2034, height: 1398), orientation: .landscapeRight, device: duo)
        #expect(turned.cutout?.edge == .top)
        #expect(turned.cutout?.inset == 120)
    }

    @Test func withoutACutoutOnlyTheGesturesRemain() {
        let none = GuestInsets.Device(cutout: nil, cutoutLength: 0, scale: 2)
        let insets = GuestInsets(safeArea: .init(top: 24, left: 0, bottom: 20, right: 0),
                                 size: PixelSize(width: 1640, height: 2360), orientation: .portrait, device: none)
        #expect(insets.cutout == nil)
        #expect(insets.systemGestures == GuestInsets.Edges(left: 60, top: 0, right: 60, bottom: 40))
    }

    @Test func theTableGivesTheHousingsEdgePerModel() {
        let duo = GuestInsets.Housing(model: "iPhone19,4")
        #expect(duo.cutout == .right)
        // The hinge's side (left) 8 pt, outside 59 pt.
        #expect(duo.corners == GuestInsets.Corners(topLeft: 8, topRight: 59, bottomRight: 59, bottomLeft: 8))
        #expect(GuestInsets.Housing(model: "iPhone16,2").cutout == .top)
        #expect(GuestInsets.Housing(model: "iPhone16,2").corners == nil)
        // An unknown iPhone gets the top, an iPad no cutout; neither has corners.
        #expect(GuestInsets.Housing(model: "iPhone42,1") == GuestInsets.Housing(model: "iPhone16,2"))
        let pad = GuestInsets.Housing(model: "iPad16,3")
        #expect(pad.cutout == nil && pad.corners == nil)
    }

    @Test func theCornersTurnWithTheInterface() {
        let housing = GuestInsets.Housing(model: "iPhone19,4")
        let duo = GuestInsets.Device(housing: housing, scale: 3)
        let portrait = GuestInsets(safeArea: .init(top: 0, left: 0, bottom: 20, right: 40),
                                   size: PixelSize(width: 1398, height: 2034), orientation: .portrait, device: duo)
        #expect(portrait.roundedCorners == GuestInsets.Corners(topLeft: 24, topRight: 177, bottomRight: 177,
                                                               bottomLeft: 24))
        // landscapeRight: the device's top left corner is the screen's bottom left, its hinge's side the bottom.
        let turned = GuestInsets(safeArea: .init(top: 40, left: 0, bottom: 20, right: 0),
                                 size: PixelSize(width: 2034, height: 1398), orientation: .landscapeRight, device: duo)
        #expect(turned.roundedCorners == GuestInsets.Corners(topLeft: 177, topRight: 177, bottomRight: 24,
                                                             bottomLeft: 24))
        let island = GuestInsets(safeArea: .init(top: 62, left: 0, bottom: 34, right: 0),
                                 size: PixelSize(width: 1206, height: 2622), orientation: .portrait,
                                 device: GuestInsets.Device(housing: GuestInsets.Housing(model: "iPhone16,2"),
                                                            scale: 3))
        #expect(island.roundedCorners == nil)
        #expect(island.cutout?.edge == .top)
    }

    @Test func aCutoutEdgeWithoutASafeAreaInsetIsNoCutout() {
        let insets = GuestInsets(safeArea: .init(top: 0, left: 0, bottom: 34, right: 0),
                                 size: PixelSize(width: 1206, height: 2622), orientation: .portrait, device: island)
        #expect(insets.cutout == nil)
    }
}
