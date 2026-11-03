import Testing

@testable import APKSession

/// ADR-0041's second windows (decision 1): the session's layers bottom to top,
/// as Android 15 orders an app's windows, with c4's surfaces beside the session's window and one dim.
@Suite("WindowOrder")
struct WindowOrderTests {
    typealias Entry = WindowOrder.Entry

    // WindowManager.LayoutParams' types.
    static let base: Int32 = 1
    static let application: Int32 = 2
    static let starting: Int32 = 3
    static let panel: Int32 = 1000
    static let media: Int32 = 1001
    static let subPanel: Int32 = 1002
    static let attachedDialog: Int32 = 1003
    static let mediaOverlay: Int32 = 1004
    static let aboveSubPanel: Int32 = 1005
    static let toast: Int32 = 2005

    @Test func theSessionsWindowAloneIsTheOnlyLayer() {
        #expect(WindowOrder.layers([]) == [.sessionWindow])
    }

    /// c4's surfaces as before: below the session's window for a negative z and above it for a positive one, by z.
    @Test func surfacesLieAroundTheSessionsWindowByZ() {
        let entries: [Entry] = [.surface(id: 11, z: 2, made: 1), .surface(id: 12, z: -1, made: 2),
                                .surface(id: 13, z: 1, made: 3), .surface(id: 14, z: -2, made: 4)]
        #expect(WindowOrder.layers(entries) == [.surface(14), .surface(12), .sessionWindow, .surface(13),
                                                .surface(11)])
    }

    /// A dialog lies above its activity's window with that window's on-top surfaces, and its popup above it.
    @Test func aDialogAndItsPopupLieAboveTheWindowAndItsSurfaces() {
        let entries: [Entry] = [.surface(id: 11, z: 1, made: 1),
                                .window(token: 7, type: Self.application, made: 2),
                                .window(token: 8, type: Self.panel, parent: 7, made: 3),
                                .surface(id: 12, z: 2, made: 4)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .surface(11), .surface(12), .window(7), .window(8)])
    }

    /// A second dialog lies above the first one and its popup.
    @Test func aNewerDialogLiesAboveTheOlderOneAndItsPopup() {
        let entries: [Entry] = [.window(token: 7, type: Self.application, made: 1),
                                .window(token: 8, type: Self.panel, parent: 7, made: 2),
                                .window(token: 9, type: Self.application, made: 3)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(7), .window(8), .window(9)])
    }

    /// A translucent activity's window starts a group above, with its own dialog; a dialog of group 0 made after
    /// them still lies below them.
    @Test func groupsLieByTheirKeyNotByMakeOrder() {
        let entries: [Entry] = [.window(token: 20, type: Self.base, group: 1, made: 1),
                                .window(token: 21, type: Self.application, group: 1, made: 2),
                                .window(token: 7, type: Self.application, made: 3)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(7), .window(20), .window(21)])
    }

    /// In a group the base window lies lowest and a starting window highest, whatever their make order
    /// (ActivityRecord.isFirstChildWindowGreaterThanSecond).
    @Test func aGroupsBaseWindowLiesLowestAndItsStartingWindowHighest() {
        let entries: [Entry] = [.window(token: 30, type: Self.starting, group: 2, made: 1),
                                .window(token: 31, type: Self.application, group: 2, made: 2),
                                .window(token: 32, type: Self.base, group: 2, made: 3)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(32), .window(31), .window(30)])
    }

    /// Toasts lie above every group, in make order.
    @Test func toastsLieAboveEveryGroup() {
        let entries: [Entry] = [.window(token: 40, type: Self.toast, made: 1),
                                .window(token: 20, type: Self.base, group: 1, made: 2),
                                .window(token: 41, type: Self.toast, made: 3)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(20), .window(40), .window(41)])
    }

    /// A sub-window's sublayer by its type, as WindowManagerPolicy.getSubWindowLayerFromTypeLw gives it, which logs
    /// an unknown type and takes 0.
    @Test func subWindowsTakeTheirTypesSublayer() {
        #expect(WindowOrder.sublayer(type: Self.media) == -2)
        #expect(WindowOrder.sublayer(type: Self.mediaOverlay) == -1)
        #expect(WindowOrder.sublayer(type: Self.panel) == 1)
        #expect(WindowOrder.sublayer(type: Self.attachedDialog) == 1)
        #expect(WindowOrder.sublayer(type: Self.subPanel) == 2)
        #expect(WindowOrder.sublayer(type: Self.aboveSubPanel) == 3)
        #expect(WindowOrder.sublayer(type: 1999) == 0)
    }

    /// A SurfaceView in a translucent activity's window: its media sub-window below the window, a panel one above,
    /// and a sublayer of 0 above the window too.
    @Test func aSurfaceViewsSubWindowsLieAroundTheirWindow() {
        let entries: [Entry] = [.window(token: 20, type: Self.base, group: 1, made: 1),
                                .window(token: 22, type: Self.panel, parent: 20, made: 2),
                                .window(token: 23, type: Self.media, parent: 20, made: 3),
                                .window(token: 24, type: 1999, parent: 20, made: 4)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(23), .window(20), .window(24), .window(22)])
    }

    /// The session's window's sub-windows mix with c4's surfaces by sublayer, then newer above.
    @Test func theSessionsSubWindowsMixWithItsSurfaces() {
        let entries: [Entry] = [.surface(id: 11, z: 1, made: 1),
                                .window(token: 8, type: Self.panel, made: 2),
                                .surface(id: 12, z: 2, made: 3),
                                .surface(id: 13, z: 1, made: 4),
                                .window(token: 9, type: Self.media, made: 5),
                                .surface(id: 14, z: -2, made: 6)]
        #expect(WindowOrder.layers(entries) == [.window(9), .surface(14), .sessionWindow, .surface(11), .window(8),
                                                .surface(13), .surface(12)])
    }

    /// One dim, below the topmost window that dims and below that window's sub-windows; the others' dims do not show.
    @Test func theDimLiesBelowTheTopmostDimmingWindowOnly() {
        let entries: [Entry] = [.window(token: 7, type: Self.application, dims: true, made: 1),
                                .window(token: 20, type: Self.base, group: 1, made: 2),
                                .window(token: 21, type: Self.application, group: 1, dims: true, made: 3),
                                .window(token: 25, type: Self.media, parent: 21, made: 4)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(7), .window(20), .dim, .window(25),
                                                .window(21)])
    }

    /// Once the topmost dimming window has gone, the dim lies below the next one down.
    @Test func theDimMovesDownWhenItsWindowGoes() {
        let entries: [Entry] = [.window(token: 7, type: Self.application, dims: true, made: 1),
                                .window(token: 20, type: Self.base, group: 1, made: 2)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .dim, .window(7), .window(20)])
    }

    @Test func noWindowDimsNoDim() {
        let entries: [Entry] = [.window(token: 7, type: Self.application, made: 1)]
        #expect(WindowOrder.layers(entries) == [.sessionWindow, .window(7)])
    }
}
