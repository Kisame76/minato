@testable import APKSession
import Testing

/// A tap outside the focused field hides the keyboard (ADR-0068 a, a named departure: Android never does): only a tap,
/// within the slop and with one finger, while the keyboard is up; at once outside a field the guest placed, and without
/// one only after the grace, unless the guest asks to show the keyboard in between.
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
}
