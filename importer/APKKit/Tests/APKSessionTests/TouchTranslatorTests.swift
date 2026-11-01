import Testing

@testable import APKSession

/// ADR-0036, decision 3: UIKit's touches as Android's motion events, with the pointer IDs and the order
/// of AOSP's TouchInputMapper (`dispatchTouches`, `dispatchMotion`) and MultiTouchInputMapper.
@Suite("TouchTranslator")
struct TouchTranslatorTests {
    static let scale = 3.0

    static func sample(
        _ x: Double, _ y: Double, at time: Double, force: Double = 0, maximumForce: Double = 0,
        radius: Double = 10
    ) -> TouchSample {
        TouchSample(x: x, y: y, force: force, maximumForce: maximumForce, majorRadius: radius, timestamp: time)
    }

    static func touch(_ key: UInt, _ x: Double, _ y: Double, at time: Double) -> TouchChange {
        TouchChange(touch: key, samples: [sample(x, y, at: time)])
    }

    /// A pointer at (x, y) in points, with no force and a radius of 10 points.
    static func pointer(_ id: Int32, _ x: Double, _ y: Double) -> MotionEvent.Pointer {
        MotionEvent.Pointer(id: id, x: Float(x * scale), y: Float(y * scale), pressure: 1, touchMajor: Float(20 * scale))
    }

    static func ns(_ seconds: Double) -> Int64 { Int64((seconds * 1e9).rounded()) }

    @Test func aTapIsDownAndUpInPixelsAndNanoseconds() {
        var translator = TouchTranslator(scale: Self.scale)

        #expect(translator.began([Self.touch(7, 10, 20, at: 100.5)]) == [
            MotionEvent(action: .down, downTime: Self.ns(100.5), eventTime: Self.ns(100.5),
                        pointers: [Self.pointer(0, 10, 20)]),
        ])
        #expect(translator.ended([Self.touch(7, 11, 21, at: 100.6)]) == [
            MotionEvent(action: .up, downTime: Self.ns(100.5), eventTime: Self.ns(100.6),
                        pointers: [Self.pointer(0, 11, 21)]),
        ])
    }

    /// UIKit gives a touch's radius and force as 0 in `touchesEnded`; Android's UP and POINTER_UP carry
    /// the pointer's last reported size and pressure, with the finger's last place.
    @Test func theLiftKeepsTheLastSizeAndPressure() {
        var translator = TouchTranslator(scale: Self.scale)
        let pressed = { (x: Double, time: Double) in Self.sample(x, 10, at: time, force: 3, maximumForce: 6, radius: 12) }
        let lifted = { (x: Double, time: Double) in Self.sample(x, 10, at: time, force: 0, maximumForce: 6, radius: 0) }
        _ = translator.began([TouchChange(touch: 1, samples: [pressed(10, 1)])])
        _ = translator.began([TouchChange(touch: 2, samples: [pressed(50, 2)])])
        let held = MotionEvent.Pointer(id: 1, x: 150, y: 30, pressure: 0.5, touchMajor: 72)

        #expect(translator.ended([TouchChange(touch: 1, samples: [lifted(11, 3)])]) == [
            MotionEvent(action: .pointerUp(0), downTime: Self.ns(1), eventTime: Self.ns(3), pointers: [
                MotionEvent.Pointer(id: 0, x: 33, y: 30, pressure: 0.5, touchMajor: 72), held,
            ]),
        ])
        #expect(translator.ended([TouchChange(touch: 2, samples: [lifted(51, 4)])]) == [
            MotionEvent(action: .up, downTime: Self.ns(1), eventTime: Self.ns(4), pointers: [
                MotionEvent.Pointer(id: 1, x: 153, y: 30, pressure: 0.5, touchMajor: 72),
            ]),
        ])
    }

    @Test func aSecondFingerIsPointerDownAndItsLiftPointerUpWithTheIndex() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])

        #expect(translator.began([Self.touch(2, 50, 50, at: 2)]) == [
            MotionEvent(action: .pointerDown(1), downTime: Self.ns(1), eventTime: Self.ns(2),
                        pointers: [Self.pointer(0, 10, 10), Self.pointer(1, 50, 50)]),
        ])
        #expect(translator.ended([Self.touch(1, 10, 10, at: 3)]) == [
            MotionEvent(action: .pointerUp(0), downTime: Self.ns(1), eventTime: Self.ns(3),
                        pointers: [Self.pointer(0, 10, 10), Self.pointer(1, 50, 50)]),
        ])
        #expect(translator.ended([Self.touch(2, 50, 50, at: 4)]) == [
            MotionEvent(action: .up, downTime: Self.ns(1), eventTime: Self.ns(4), pointers: [Self.pointer(1, 50, 50)]),
        ])
    }

    /// MultiTouchInputMapper takes the lowest free ID; dispatchMotion orders the pointers by ID.
    @Test func aNewFingerTakesTheLowestFreeIDAndItsPlaceInTheOrder() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.began([Self.touch(2, 50, 50, at: 2)])
        _ = translator.ended([Self.touch(1, 10, 10, at: 3)])

        #expect(translator.began([Self.touch(3, 30, 30, at: 4)]) == [
            MotionEvent(action: .pointerDown(0), downTime: Self.ns(1), eventTime: Self.ns(4),
                        pointers: [Self.pointer(0, 30, 30), Self.pointer(1, 50, 50)]),
        ])
    }

    @Test func aMoveCarriesEveryPointer() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.began([Self.touch(2, 50, 50, at: 2)])

        #expect(translator.moved([Self.touch(2, 60, 70, at: 3)]) == [
            MotionEvent(action: .move, downTime: Self.ns(1), eventTime: Self.ns(3),
                        pointers: [Self.pointer(0, 10, 10), Self.pointer(1, 60, 70)]),
        ])
    }

    @Test func theCoalescedSamplesBecomeTheHistory() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        let change = TouchChange(touch: 1, samples: [
            Self.sample(11, 10, at: 1.004), Self.sample(12, 10, at: 1.008), Self.sample(13, 10, at: 1.012),
        ])

        #expect(translator.moved([change]) == [
            MotionEvent(action: .move, downTime: Self.ns(1), eventTime: Self.ns(1.012),
                        pointers: [Self.pointer(0, 13, 10)],
                        history: [
                            .init(time: Self.ns(1.004), pointers: [Self.pointer(0, 11, 10)]),
                            .init(time: Self.ns(1.008), pointers: [Self.pointer(0, 12, 10)]),
                        ]),
        ])
    }

    /// Each historical sample holds every pointer, as Android's; a pointer without a sample at that time
    /// keeps the place it had.
    @Test func theHistoryOfSeveralPointersHoldsEveryPointer() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.began([Self.touch(2, 50, 50, at: 1)])
        _ = translator.began([Self.touch(3, 90, 90, at: 1)])
        let first = TouchChange(touch: 1, samples: [
            Self.sample(11, 10, at: 2.004), Self.sample(12, 10, at: 2.008), Self.sample(13, 10, at: 2.012),
        ])
        let second = TouchChange(touch: 2, samples: [Self.sample(51, 50, at: 2.008), Self.sample(52, 50, at: 2.012)])

        #expect(translator.moved([second, first]) == [
            MotionEvent(action: .move, downTime: Self.ns(1), eventTime: Self.ns(2.012),
                        pointers: [Self.pointer(0, 13, 10), Self.pointer(1, 52, 50), Self.pointer(2, 90, 90)],
                        history: [
                            .init(time: Self.ns(2.004),
                                  pointers: [Self.pointer(0, 11, 10), Self.pointer(1, 50, 50), Self.pointer(2, 90, 90)]),
                            .init(time: Self.ns(2.008),
                                  pointers: [Self.pointer(0, 12, 10), Self.pointer(1, 51, 50), Self.pointer(2, 90, 90)]),
                        ]),
        ])
    }

    @Test func pressureIsTheShareOfTheMaximumForceAndOneWithoutForce() {
        var translator = TouchTranslator(scale: 2)
        let pressed = TouchChange(touch: 1, samples: [Self.sample(0, 0, at: 1, force: 3, maximumForce: 6, radius: 15)])

        let pointer = translator.began([pressed]).first?.pointers.first
        #expect(pointer?.pressure == 0.5)
        #expect(pointer?.touchMajor == 60)
        #expect(translator.began([Self.touch(2, 0, 0, at: 2)]).first?.pointers.last?.pressure == 1)
    }

    /// dispatchTouches sends one event per pointer that goes down or up, in the order of the IDs.
    @Test func fingersThatComeOrGoTogetherGetAnEventEach() {
        var translator = TouchTranslator(scale: Self.scale)

        #expect(translator.began([Self.touch(1, 10, 10, at: 1), Self.touch(2, 50, 50, at: 1)]).map(\.action) == [
            .down, .pointerDown(1),
        ])
        let events = translator.ended([Self.touch(2, 50, 50, at: 2), Self.touch(1, 10, 10, at: 2)])
        #expect(events.map(\.action) == [.pointerUp(0), .up])
        #expect(events.map(\.pointers.count) == [2, 1])
        #expect(events.last?.pointers.first?.id == 1)
    }

    /// At the top-edge pan UIKit cancels the pan's touches; the whole gesture ends with one CANCEL, and the
    /// other fingers are ignored until they lift.
    @Test func aCancelledTouchCancelsTheWholeGesture() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.began([Self.touch(2, 50, 50, at: 2)])

        #expect(translator.cancelled([Self.touch(1, 10, 5, at: 3)]) == [
            MotionEvent(action: .cancel, downTime: Self.ns(1), eventTime: Self.ns(3),
                        pointers: [Self.pointer(0, 10, 10), Self.pointer(1, 50, 50)]),
        ])
        #expect(translator.moved([Self.touch(2, 60, 60, at: 4)]) == [])
        #expect(translator.ended([Self.touch(2, 60, 60, at: 5)]) == [])
        #expect(translator.began([Self.touch(3, 30, 30, at: 6)]) == [
            MotionEvent(action: .down, downTime: Self.ns(6), eventTime: Self.ns(6), pointers: [Self.pointer(0, 30, 30)]),
        ])
    }

    @Test func aFingerThatStaysDownAfterACancelStartsNothingNew() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.began([Self.touch(2, 50, 50, at: 2)])
        _ = translator.cancelled([Self.touch(1, 10, 10, at: 3)])

        #expect(translator.began([Self.touch(3, 30, 30, at: 4)]) == [
            MotionEvent(action: .down, downTime: Self.ns(4), eventTime: Self.ns(4), pointers: [Self.pointer(0, 30, 30)]),
        ])
        #expect(translator.moved([Self.touch(2, 60, 60, at: 5)]) == [])
    }

    @Test func losingTheFocusCancelsAnOpenGesture() {
        var translator = TouchTranslator(scale: Self.scale)
        #expect(translator.focusLost(at: 1) == [])

        _ = translator.began([Self.touch(1, 10, 10, at: 2)])
        #expect(translator.focusLost(at: 3) == [
            MotionEvent(action: .cancel, downTime: Self.ns(2), eventTime: Self.ns(3), pointers: [Self.pointer(0, 10, 10)]),
        ])
        #expect(translator.ended([Self.touch(1, 10, 10, at: 4)]) == [])
    }

    /// Android's MAX_POINTERS is 16; further fingers are ignored.
    @Test func fingersBeyondSixteenAreIgnored() {
        var translator = TouchTranslator(scale: Self.scale)
        let touches = (0..<17).map { Self.touch(UInt($0), Double($0), 0, at: 1) }

        let events = translator.began(touches)
        #expect(events.count == 16)
        #expect(events.last?.action == .pointerDown(15))
        #expect(translator.moved([Self.touch(16, 5, 5, at: 2)]) == [])
        #expect(translator.ended([Self.touch(16, 5, 5, at: 3)]) == [])
    }

    /// The view hands every callback over with its phase.
    @Test func eachPhaseGoesToItsCallback() {
        var translator = TouchTranslator(scale: Self.scale)

        #expect(translator.translate(.began, [Self.touch(1, 10, 10, at: 1)]).map(\.action) == [.down])
        #expect(translator.translate(.moved, [Self.touch(1, 20, 20, at: 2)]).map(\.action) == [.move])
        #expect(translator.translate(.ended, [Self.touch(1, 20, 20, at: 3)]).map(\.action) == [.up])
        _ = translator.translate(.began, [Self.touch(2, 10, 10, at: 4)])
        #expect(translator.translate(.cancelled, [Self.touch(2, 10, 10, at: 5)]).map(\.action) == [.cancel])
    }

    // ADR-0041, decision 12 b: in size compat the guest's window shows in a frame of the view, scaled as Android 15
    // scales it (WindowState's global scale): its touches come in the guest's pixels.

    static let scaledDown = Letterbox(frame: PixelRect(left: 100, top: 0, right: 600, bottom: 1000), scale: 0.5,
                                      size: PixelSize(width: 1000, height: 2000))

    /// From the frame's corner, by the inverse of the scale; the touch's size too.
    @Test func inSizeCompatTouchesComeInTheGuestsPixels() {
        var translator = TouchTranslator(scale: Self.scale)
        translator.letterbox = Self.scaledDown

        #expect(translator.began([Self.touch(1, 100, 50, at: 1)]) == [
            MotionEvent(action: .down, downTime: Self.ns(1), eventTime: Self.ns(1),
                        pointers: [MotionEvent.Pointer(id: 0, x: 400, y: 300, pressure: 1, touchMajor: 120)]),
        ])
    }

    /// A touch that begins on the letterbox is not the guest's, to its end, as Android's letterbox takes it.
    @Test func aTouchThatBeginsOnTheLetterboxIsNotTheGuests() {
        var translator = TouchTranslator(scale: Self.scale)
        translator.letterbox = Self.scaledDown

        #expect(translator.began([Self.touch(1, 10, 50, at: 1)]) == [])
        #expect(translator.began([Self.touch(2, 100, 400, at: 1)]) == [])
        #expect(translator.moved([Self.touch(1, 100, 50, at: 2)]) == [])
        #expect(translator.ended([Self.touch(1, 100, 50, at: 3)]) == [])
    }

    /// A touch that begins in the frame stays the guest's when it leaves it, outside the guest's window.
    @Test func aTouchThatLeavesTheFrameStaysTheGuests() {
        var translator = TouchTranslator(scale: Self.scale)
        translator.letterbox = Self.scaledDown
        _ = translator.began([Self.touch(1, 100, 50, at: 1)])

        #expect(translator.moved([Self.touch(1, 10, 50, at: 2)]).first?.pointers.first?.x == -140)
    }

    /// A touch whose object address UIKit reuses is a new touch.
    @Test func aTouchThatBeginsAgainIsNew() {
        var translator = TouchTranslator(scale: Self.scale)
        _ = translator.began([Self.touch(1, 10, 10, at: 1)])
        _ = translator.focusLost(at: 2)

        #expect(translator.began([Self.touch(1, 20, 20, at: 3)]).map(\.action) == [.down])
    }
}
