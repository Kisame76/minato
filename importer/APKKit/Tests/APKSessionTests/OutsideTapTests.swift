@testable import APKSession
import Testing

/// A tap outside the focused field hides the keyboard (ADR-0068 a, a named departure: Android never does): only a tap,
/// within the slop and with one finger, while the keyboard is up; at once outside a field the guest placed, and without
/// one only after the grace, unless the guest asks to show the keyboard in between. The tap that hides never reaches
/// the guest: a one-finger touch waits while the keyboard is up until it is known.
@Suite struct OutsideTapTests {
    static let field = FieldRect(left: 0, top: 100, right: 400, bottom: 160)

    static func change(_ touch: UInt, _ x: Double, _ y: Double) -> TouchChange {
        TouchChange(touch: touch, samples: [TouchSample(x: x, y: y, force: 0, maximumForce: 0, majorRadius: 10,
                                                        timestamp: 0)])
    }

    /// One finger down at (x, y), then up at (x + dx, y + dy).
    static func tap(_ tap: inout OutsideTap, _ x: Double, _ y: Double, dx: Double = 0, dy: Double = 0,
                    shown: Bool = true) -> OutsideTap.Decision {
        _ = tap.touched(.began, [change(1, x, y)], keyboardShown: shown)
        if dx != 0 || dy != 0 {
            _ = tap.touched(.moved, [change(1, x + dx, y + dy)], keyboardShown: shown)
        }
        return tap.touched(.ended, [change(1, x + dx, y + dy)], keyboardShown: shown)
    }

    /// A field without extract UI (SDL2's, 0x91 with IME_FLAG_NO_EXTRACT_UI) keeps the keyboard at a tap outside, as
    /// Android 15 does (Planung's Ren'Py run): the tap reaches the guest at once; every other field's keyboard goes.
    @Test func aFieldWithoutExtractUIKeepsTheKeyboardAndTheTapReachesTheGuest() {
        var keyboard = SoftKeyboard()
        _ = keyboard.handle(.start(serial: 5, editor: EditorSpec(
            inputType: 0x91, imeOptions: 0x1200_0000, capsMode: 0, selectionStart: -1, selectionEnd: -1, hint: "")))
        #expect(keyboard.keepsKeyboardAtOutsideTap)
        var tap = OutsideTap()
        tap.field = Self.field
        tap.keepsKeyboard = keyboard.keepsKeyboardAtOutsideTap
        _ = tap.touched(.began, [Self.change(1, 200, 400)], keyboardShown: true)
        #expect(tap.release().map(\.phase) == [.began])
        #expect(tap.touched(.ended, [Self.change(1, 200, 400)], keyboardShown: true) == .none)
        #expect(tap.release().map(\.phase) == [.ended])

        var plain = SoftKeyboard()
        _ = plain.handle(.start(serial: 5, editor: EditorSpec(
            inputType: 0x1, imeOptions: 0, capsMode: 0, selectionStart: 0, selectionEnd: 0, hint: "")))
        #expect(!plain.keepsKeyboardAtOutsideTap)
        #expect(!SoftKeyboard().keepsKeyboardAtOutsideTap)
        var hides = OutsideTap()
        hides.field = Self.field
        hides.keepsKeyboard = plain.keepsKeyboardAtOutsideTap
        #expect(Self.tap(&hides, 200, 400) == .hide)
    }

    @Test func aTapOutsideThePlacedFieldHidesAtOnce() {
        var tap = OutsideTap()
        tap.field = Self.field
        let r1 = Self.tap(&tap, 200, 400)
        #expect(r1 == .hide)
    }

    @Test func aTapOnThePlacedFieldKeepsTheKeyboard() {
        var tap = OutsideTap()
        tap.field = Self.field
        let r2 = Self.tap(&tap, 200, 130)
        #expect(r2 == .none)
    }

    @Test func aDragOrAScrollKeepsTheKeyboard() {
        var tap = OutsideTap()
        tap.field = Self.field
        let r3 = Self.tap(&tap, 200, 400, dy: -60)
        #expect(r3 == .none)
        var unplaced = OutsideTap()
        let r4 = Self.tap(&unplaced, 200, 400, dx: 30)
        #expect(r4 == .none)
    }

    @Test func aMoveWithinTheSlopIsStillATap() {
        var tap = OutsideTap()
        tap.field = Self.field
        let r5 = Self.tap(&tap, 200, 400, dx: 4, dy: -6)
        #expect(r5 == .hide)
    }

    @Test func withoutAFieldTheHideWaitsForTheGrace() {
        var tap = OutsideTap()
        guard case .hideAfterGrace(let token) = Self.tap(&tap, 200, 400) else {
            Issue.record("no hide after the grace")
            return
        }
        let r6 = tap.graceEnded(token)
        #expect(r6)
        let r7 = tap.graceEnded(token)
        #expect(!r7)
    }

    @Test func aShowInTheGraceCancelsTheHide() {
        var tap = OutsideTap()
        guard case .hideAfterGrace(let token) = Self.tap(&tap, 200, 400) else {
            Issue.record("no hide after the grace")
            return
        }
        tap.showRequested()
        let r8 = tap.graceEnded(token)
        #expect(!r8)
    }

    @Test func aShowDuringTheTouchCancelsTheHide() {
        var tap = OutsideTap()
        _ = tap.touched(.began, [Self.change(1, 200, 400)], keyboardShown: true)
        tap.showRequested()
        let r9 = tap.touched(.ended, [Self.change(1, 200, 400)], keyboardShown: true)
        #expect(r9 == .none)
    }

    @Test func aNewTapCancelsAnEarlierGrace() {
        var tap = OutsideTap()
        guard case .hideAfterGrace(let first) = Self.tap(&tap, 200, 400) else {
            Issue.record("no hide after the grace")
            return
        }
        guard case .hideAfterGrace(let second) = Self.tap(&tap, 220, 420) else {
            Issue.record("no hide after the second grace")
            return
        }
        let r10 = tap.graceEnded(first)
        #expect(!r10)
        let r11 = tap.graceEnded(second)
        #expect(r11)
    }

    @Test func nothingHidesWhileTheKeyboardIsDown() {
        var tap = OutsideTap()
        tap.field = Self.field
        let r12 = Self.tap(&tap, 200, 400, shown: false)
        #expect(r12 == .none)
    }

    @Test func aSecondFingerOrACancelIsNoTap() {
        var tap = OutsideTap()
        tap.field = Self.field
        _ = tap.touched(.began, [Self.change(1, 200, 400)], keyboardShown: true)
        _ = tap.touched(.began, [Self.change(2, 300, 500)], keyboardShown: true)
        _ = tap.touched(.ended, [Self.change(2, 300, 500)], keyboardShown: true)
        let r13 = tap.touched(.ended, [Self.change(1, 200, 400)], keyboardShown: true)
        #expect(r13 == .none)
        _ = tap.touched(.began, [Self.change(3, 200, 400)], keyboardShown: true)
        let r14 = tap.touched(.cancelled, [Self.change(3, 200, 400)], keyboardShown: true)
        #expect(r14 == .none)
        let r15 = Self.tap(&tap, 200, 400)
        #expect(r15 == .hide)
    }

    static func callback(_ phase: TouchPhase, _ touch: UInt, _ x: Double, _ y: Double) -> OutsideTap.Callback {
        OutsideTap.Callback(phase: phase, changes: [change(touch, x, y)])
    }

    /// The touch callback goes in, and what reaches the guest now comes out.
    static func pass(_ tap: inout OutsideTap, _ callback: OutsideTap.Callback, shown: Bool = true)
        -> [OutsideTap.Callback] {
        _ = tap.touched(callback.phase, callback.changes, keyboardShown: shown)
        return tap.release()
    }

    @Test func theTapThatHidesNeverReachesTheGuest() {
        // A game that asks for its keyboard on every touch would show it again at once.
        var placed = OutsideTap()
        placed.field = Self.field
        var unplaced = OutsideTap()
        for tap in [placed, unplaced] {
            var tap = tap
            let down = Self.pass(&tap, Self.callback(.began, 1, 200, 400))
            #expect(down.isEmpty)
            let up = Self.pass(&tap, Self.callback(.ended, 1, 202, 401))
            #expect(up.isEmpty)
        }
        _ = unplaced.touched(.began, [Self.change(1, 200, 400)], keyboardShown: true)
        #expect(unplaced.release().isEmpty)
    }

    @Test func aTouchThatIsNoTapGoesOutLateInOrder() {
        var tap = OutsideTap()
        tap.field = Self.field
        let down = Self.callback(.began, 1, 200, 400)
        let near = Self.callback(.moved, 1, 203, 404)
        let far = Self.callback(.moved, 1, 200, 340)
        #expect(Self.pass(&tap, down).isEmpty)
        #expect(Self.pass(&tap, near).isEmpty)
        #expect(Self.pass(&tap, far) == [down, near, far])
        let up = Self.callback(.ended, 1, 200, 340)
        #expect(Self.pass(&tap, up) == [up])
    }

    @Test func aSecondFingerSendsTheHeldTouch() {
        var tap = OutsideTap()
        let down = Self.callback(.began, 1, 200, 400)
        let second = Self.callback(.began, 2, 300, 500)
        #expect(Self.pass(&tap, down).isEmpty)
        #expect(Self.pass(&tap, second) == [down, second])
    }

    @Test func aTapOnThePlacedFieldReachesIt() {
        var tap = OutsideTap()
        tap.field = Self.field
        let down = Self.callback(.began, 1, 200, 130)
        let up = Self.callback(.ended, 1, 200, 130)
        #expect(Self.pass(&tap, down) == [down])
        #expect(Self.pass(&tap, up) == [up])
    }

    @Test func withoutTheKeyboardNothingWaits() {
        var tap = OutsideTap()
        let down = Self.callback(.began, 1, 200, 400)
        let up = Self.callback(.ended, 1, 200, 400)
        #expect(Self.pass(&tap, down, shown: false) == [down])
        #expect(Self.pass(&tap, up, shown: false) == [up])
    }

    @Test func aCancelledHeldTouchNeverReachesTheGuest() {
        var tap = OutsideTap()
        #expect(Self.pass(&tap, Self.callback(.began, 1, 200, 400)).isEmpty)
        #expect(Self.pass(&tap, Self.callback(.cancelled, 1, 200, 400)).isEmpty)
    }
}
