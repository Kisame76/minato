import Testing

@testable import APKSession

/// ADR-0068 decision 5, the mouse step: UIKit's pointer (an indirect pointer's touches, the hover, the scroll pan and
/// the event's button mask) as the reports tools/mouse-check.sh's mouse: steps post through apkrt_input_post_mouse,
/// which give mouse-test the lines Android 15 logs for a uinput mouse.
@Suite struct MouseTests {
    private func reports(_ events: [PadEvent]) -> [String] {
        events.map { event in
            switch event {
            case .deviceAdded(let device): "add \(device.id) \(String(device.sources, radix: 16))"
            case .deviceRemoved(let device): "remove \(device.id)"
            case .key(let key): "key \(key.keyCode)"
            case .axes(let record):
                "\(record.action) b\(record.buttonState) t\(record.toolType) s\(String(record.source, radix: 16)) "
                    + record.axes.map { "\($0.axis)=\($0.value)" }.joined(separator: " ")
            }
        }
    }

    @Test func theFirstReportListsTheMouseAndAMoveIsAHoverMoveInTheWindowsPixels() {
        var mouse = Mouse(scale: 2)
        #expect(reports(mouse.moved(x: 315, y: 610, time: 1)) == ["add 1001 2002", "7 b0 t3 s2002 0=630.0 1=1220.0"])
        #expect(reports(mouse.moved(x: 320, y: 610, time: 2)) == ["7 b0 t3 s2002 0=640.0 1=1220.0 27=10.0"])
        #expect(mouse.moved(x: 320, y: 610, time: 3).isEmpty)
    }

    @Test func eachButtonThatChangesIsAPressOrReleaseWithTheButtonsAfterIt() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 10, y: 20, time: 1)
        #expect(reports(mouse.buttons(1, x: 10, y: 20, time: 2)) == ["11 b1 t3 s2002 0=10.0 1=20.0"])
        #expect(reports(mouse.buttons(3, x: 10, y: 20, time: 3)) == ["11 b3 t3 s2002 0=10.0 1=20.0"])
        #expect(reports(mouse.buttons(1, x: 10, y: 20, time: 4)) == ["12 b1 t3 s2002 0=10.0 1=20.0"])
        #expect(reports(mouse.buttons(0, x: 10, y: 20, time: 5)) == ["12 b0 t3 s2002 0=10.0 1=20.0"])
        #expect(mouse.buttons(0, x: 10, y: 20, time: 6).isEmpty)
    }

    @Test func twoButtonsAtOnceAreTwoReportsLowestFirst() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        #expect(reports(mouse.buttons(3, x: 1, y: 1, time: 2)) == ["11 b1 t3 s2002 0=1.0 1=1.0",
                                                                   "11 b3 t3 s2002 0=1.0 1=1.0"])
    }

    @Test func twoButtonsLetGoAtOnceAreTwoReleasesLowestFirst() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        _ = mouse.buttons(3, x: 1, y: 1, time: 2)
        #expect(reports(mouse.buttons(0, x: 1, y: 1, time: 3)) == ["12 b2 t3 s2002 0=1.0 1=1.0",
                                                                   "12 b0 t3 s2002 0=1.0 1=1.0"])
    }

    @Test func aDragMovesWithTheButtonHeldAndAClickWhereThePointerIsNotYetMovesFirst() {
        var mouse = Mouse(scale: 2)
        _ = mouse.moved(x: 320, y: 610, time: 1)
        #expect(reports(mouse.buttons(1, x: 320, y: 610, time: 2)) == ["11 b1 t3 s2002 0=640.0 1=1220.0"])
        #expect(reports(mouse.moved(x: 325, y: 615, time: 3)) == ["7 b1 t3 s2002 0=650.0 1=1230.0 27=10.0 28=10.0"])
        #expect(reports(mouse.buttons(0, x: 330, y: 615, time: 4)) == ["7 b1 t3 s2002 0=660.0 1=1230.0 27=10.0",
                                                                       "12 b0 t3 s2002 0=660.0 1=1230.0"])
    }

    /// While a button is held only touchesMoved reports the pointer, as the same mask: each is a move with the button
    /// held, so that a drag is no click (text selection, sliders, maps, painting).
    @Test func theSameButtonsAtAnotherPlaceAreADragsMove() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 10, y: 20, time: 1)
        #expect(reports(mouse.buttons(1, x: 10, y: 20, time: 2)) == ["11 b1 t3 s2002 0=10.0 1=20.0"])
        #expect(reports(mouse.buttons(1, x: 15, y: 20, time: 3)) == ["7 b1 t3 s2002 0=15.0 1=20.0 27=5.0"])
        #expect(reports(mouse.buttons(1, x: 15, y: 30, time: 4)) == ["7 b1 t3 s2002 0=15.0 1=30.0 28=10.0"])
        #expect(mouse.buttons(1, x: 15, y: 30, time: 5).isEmpty)
        #expect(reports(mouse.buttons(0, x: 15, y: 30, time: 6)) == ["12 b0 t3 s2002 0=15.0 1=30.0"])
    }

    @Test func backAndForwardAreButtons8And16() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        #expect(reports(mouse.buttons(8, x: 1, y: 1, time: 2)) == ["11 b8 t3 s2002 0=1.0 1=1.0"])
        // Releases before presses, each with the buttons after it.
        #expect(reports(mouse.buttons(16, x: 1, y: 1, time: 3)) == ["12 b0 t3 s2002 0=1.0 1=1.0",
                                                                   "11 b16 t3 s2002 0=1.0 1=1.0"])
    }

    /// UIKit's translation moves with the content: a pan to the right shows what lies to the left, which Android's
    /// AXIS_HSCROLL calls left (-1.0 left to 1.0 right, so HorizontalScrollView's scrollX goes down); a pan down shows
    /// what lies above, AXIS_VSCROLL's up (-1.0 down to 1.0 up).
    @Test func aScrollIsInLinesOf64PointsPositiveUpAndRight() {
        var mouse = Mouse(scale: 3)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        #expect(reports(mouse.scrolled(dx: 0, dy: 64, time: 2)) == ["8 b0 t3 s2002 9=1.0"])
        #expect(reports(mouse.scrolled(dx: 0, dy: -128, time: 3)) == ["8 b0 t3 s2002 9=-2.0"])
        #expect(reports(mouse.scrolled(dx: 32, dy: 0, time: 4)) == ["8 b0 t3 s2002 10=-0.5"])
        #expect(reports(mouse.scrolled(dx: -128, dy: 0, time: 5)) == ["8 b0 t3 s2002 10=2.0"])
        #expect(mouse.scrolled(dx: 0, dy: 0, time: 5).isEmpty)
    }

    @Test func theFirstScrollListsTheMouse() {
        var mouse = Mouse(scale: 1)
        #expect(reports(mouse.scrolled(dx: 0, dy: 64, time: 1)) == ["add 1001 2002", "8 b0 t3 s2002 9=1.0"])
    }

    @Test func anExitIsAHoverExitOnceAndTheNextMoveEntersAgain() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        #expect(reports(mouse.exited(time: 2)) == ["10 b0 t3 s2002 "])
        #expect(mouse.exited(time: 3).isEmpty)
        #expect(reports(mouse.moved(x: 2, y: 1, time: 4)) == ["7 b0 t3 s2002 0=2.0 1=1.0"])
    }

    @Test func theHoversEndAtAClickIsNoExitWhileAButtonIsHeld() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        _ = mouse.buttons(1, x: 1, y: 1, time: 2)
        #expect(mouse.exited(time: 3).isEmpty)
        #expect(reports(mouse.moved(x: 3, y: 1, time: 4)) == ["7 b1 t3 s2002 0=3.0 1=1.0 27=2.0"])
    }

    @Test func aDisconnectReleasesTheButtonsExitsAndRemovesTheMouse() {
        var mouse = Mouse(scale: 1)
        _ = mouse.moved(x: 1, y: 1, time: 1)
        _ = mouse.buttons(1, x: 1, y: 1, time: 2)
        #expect(reports(mouse.disconnect(time: 3)) == ["12 b0 t3 s2002 0=1.0 1=1.0", "10 b0 t3 s2002 ", "remove 1001"])
        var unused = Mouse(scale: 1)
        #expect(unused.disconnect(time: 1).isEmpty)
    }

    @Test func inSizeCompatThePointerIsInTheGuestsFrame() {
        var mouse = Mouse(scale: 2)
        mouse.letterbox = Letterbox(frame: PixelRect(left: 100, top: 0, right: 1100, bottom: 2000), scale: 0.5,
                                    size: PixelSize(width: 1000, height: 2000))
        #expect(reports(mouse.moved(x: 100, y: 50, time: 1)).last == "7 b0 t3 s2002 0=200.0 1=200.0")
    }
}
