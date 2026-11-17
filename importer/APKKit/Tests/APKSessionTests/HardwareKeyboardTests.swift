import Testing

@testable import APKSession

/// ADR-0068 step 7c2: UIKit's hardware key presses as Android's key events, in the order of AOSP's
/// KeyboardInputMapper, and the keyboard's name as the input channel takes it.
@Suite("HardwareKeyboard")
struct HardwareKeyboardTests {
    // HID usages (UIKeyboardHIDUsage) and Android's key codes, as KeyEvent.java has them.
    static let a = 0x04, one = 0x1e, enter = 0x28, leftShift = 0xe1, rightShift = 0xe5, leftControl = 0xe0
    static let capsLock = 0x39, leftArrow = 0x50

    private func keys(_ events: [KeyboardEvent]) -> [KeyRecord] {
        events.compactMap { if case .key(let record) = $0 { record } else { nil } }
    }

    @Test func aLetterIsKeyCodeAAndLeavesWithItsUp() {
        var keyboard = HardwareKeyboard()
        let down = keys(keyboard.press(usage: Self.a, down: true, time: 1_000))
        let up = keys(keyboard.press(usage: Self.a, down: false, time: 2_000))

        #expect(down.count == 1 && up.count == 1)
        #expect(down[0].action == 0 && up[0].action == 1)
        #expect(down[0].keyCode == 29 && up[0].keyCode == 29)
        #expect(down[0].downTime == 1_000 && up[0].downTime == 1_000)
        #expect(down[0].eventTime == 1_000 && up[0].eventTime == 2_000)
        #expect(down[0].deviceID == HardwareKeyboard.deviceID)
        #expect(down[0].source == 0x101)
        #expect(down[0].metaState == 0 && down[0].repeatCount == 0)
    }

    @Test func theFirstKeyAddsTheKeyboardOnceAndFirst() {
        var keyboard = HardwareKeyboard()
        let first = keyboard.press(usage: Self.a, down: true, time: 1)
        let second = keyboard.press(usage: Self.a, down: false, time: 2)

        guard case .deviceAdded(let device) = first.first else {
            Issue.record("the first event is not the device")
            return
        }
        #expect(device.id == 1000 && device.name == "Hardware Keyboard" && device.sources == 0x101)
        #expect(first.count == 2)
        #expect(second.count == 1)
    }

    @Test func aKeyThatHasNoAndroidCodeIsIgnoredAndAddsNothing() {
        var keyboard = HardwareKeyboard()
        #expect(keyboard.press(usage: 0x7fff, down: true, time: 1).isEmpty)
        #expect(keyboard.press(usage: 0x7fff, down: false, time: 2).isEmpty)
        // The keyboard is still new: the next known key adds it.
        #expect(keyboard.press(usage: Self.a, down: true, time: 3).count == 2)
    }

    @Test func shiftIsInTheMetaStateOfTheKeysWhileItIsHeldAndOfItsOwnDown() {
        var keyboard = HardwareKeyboard()
        let shiftDown = keys(keyboard.press(usage: Self.leftShift, down: true, time: 1))
        let aDown = keys(keyboard.press(usage: Self.a, down: true, time: 2))
        _ = keyboard.press(usage: Self.a, down: false, time: 3)
        _ = keyboard.press(usage: Self.leftShift, down: false, time: 4)
        let after = keys(keyboard.press(usage: Self.a, down: true, time: 5))

        // META_SHIFT_ON 0x1 | META_SHIFT_LEFT_ON 0x40
        #expect(shiftDown[0].keyCode == 59 && shiftDown[0].metaState == 0x41)
        #expect(aDown[0].metaState == 0x41)
        #expect(after[0].metaState == 0)
    }

    @Test func bothShiftsKeepShiftOnUntilTheLastGoesUp() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.leftShift, down: true, time: 1)
        _ = keyboard.press(usage: Self.rightShift, down: true, time: 2)
        let leftUp = keys(keyboard.press(usage: Self.leftShift, down: false, time: 3))
        let a = keys(keyboard.press(usage: Self.a, down: true, time: 4))

        #expect(leftUp[0].metaState == 0x81)  // the state after the up: the right side still holds Shift
        #expect(a[0].metaState == 0x81)  // META_SHIFT_ON | META_SHIFT_RIGHT_ON
    }

    @Test func aModifiersUpCarriesTheStateWithoutIt() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.leftShift, down: true, time: 1)
        let up = keys(keyboard.press(usage: Self.leftShift, down: false, time: 2))

        #expect(up[0].keyCode == 59 && up[0].action == 1)
        #expect(up[0].metaState == 0)  // as KeyboardInputMapper sends it: updated before the event
    }

    @Test func aControlUpCarriesTheStateWithoutItButTheCapsLockStays() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.capsLock, down: true, time: 1)
        _ = keyboard.press(usage: Self.leftControl, down: true, time: 2)
        let up = keys(keyboard.press(usage: Self.leftControl, down: false, time: 3))

        #expect(up[0].metaState == 0x100000)  // META_CAPS_LOCK_ON alone
    }

    @Test func controlSetsItsBitsAndCapsLockToggles() {
        var keyboard = HardwareKeyboard()
        let control = keys(keyboard.press(usage: Self.leftControl, down: true, time: 1))
        let caps = keys(keyboard.press(usage: Self.capsLock, down: true, time: 2))
        _ = keyboard.press(usage: Self.capsLock, down: false, time: 3)
        let a = keys(keyboard.press(usage: Self.a, down: true, time: 4))
        _ = keyboard.press(usage: Self.capsLock, down: true, time: 5)
        _ = keyboard.press(usage: Self.capsLock, down: false, time: 6)
        let off = keys(keyboard.press(usage: Self.a, down: true, time: 7))

        // META_CTRL_ON 0x1000 | META_CTRL_LEFT_ON 0x2000; META_CAPS_LOCK_ON 0x100000
        #expect(control[0].metaState == 0x3000)
        #expect(caps[0].metaState == 0x103000)
        #expect(a[0].metaState == 0x103000)
        #expect(off[0].metaState == 0x3000)
    }

    @Test func aKeyHeldDownRepeatsWithACount() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.a, down: true, time: 1)
        let repeated = keys(keyboard.press(usage: Self.a, down: true, time: 2))
        let again = keys(keyboard.press(usage: Self.a, down: true, time: 3))

        #expect(repeated[0].action == 0 && repeated[0].repeatCount == 1)
        #expect(again[0].repeatCount == 2)
        #expect(repeated[0].downTime == 1 && repeated[0].eventTime == 2)
    }

    @Test func anUpWithoutADownIsIgnored() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.a, down: true, time: 1)
        _ = keyboard.press(usage: Self.a, down: false, time: 2)
        #expect(keyboard.press(usage: Self.a, down: false, time: 3).isEmpty)
    }

    @Test func releaseAllEndsTheHeldKeysInTheOrderTheyWentDown() {
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: Self.leftShift, down: true, time: 1)
        _ = keyboard.press(usage: Self.a, down: true, time: 2)
        let released = keys(keyboard.releaseAll(time: 9))

        #expect(released.map(\.keyCode) == [59, 29])
        #expect(released.allSatisfy { $0.action == 1 && $0.eventTime == 9 })
        #expect(keyboard.releaseAll(time: 10).isEmpty)
        #expect(keys(keyboard.press(usage: Self.a, down: true, time: 11))[0].metaState == 0)
    }

    @Test func theTableHasAndroidsCodesForTheCommonKeys() {
        let table: [(Int, Int32)] = [
            (0x04, 29), (0x1d, 54), (0x1e, 8), (0x26, 16), (0x27, 7), (0x28, 66), (0x29, 111), (0x2a, 67),
            (0x2b, 61), (0x2c, 62), (0x2d, 69), (0x2e, 70), (0x2f, 71), (0x30, 72), (0x31, 73), (0x33, 74),
            (0x34, 75), (0x35, 68), (0x36, 55), (0x37, 56), (0x38, 76), (0x3a, 131), (0x45, 142), (0x4a, 122),
            (0x4b, 92), (0x4c, 112), (0x4d, 123), (0x4e, 93), (0x4f, 22), (0x50, 21), (0x51, 20), (0x52, 19),
            (0x59, 145), (0x62, 144), (0xe0, 113), (0xe1, 59), (0xe2, 57), (0xe3, 117), (0xe7, 118),
        ]
        for (usage, code) in table {
            #expect(HardwareKeyboard.androidKeyCode(forHIDUsage: usage) == code, "usage \(usage)")
        }
        #expect(HardwareKeyboard.androidKeyCode(forHIDUsage: 0) == nil)
    }

    // The name goes to apkrt_input_post_device, which takes only UTF-8 that Java's JNI takes as it is: a name keeps
    // its Basic Multilingual Plane characters, a character outside the plane becomes the replacement character, and a
    // NUL ends the name.
    @Test(arguments: [
        ("Tastatur é 日本", "Tastatur é 日本"),
        ("Pad 😀 Pro", "Pad \u{fffd} Pro"),
        ("Pad\0Pro", "Pad"),
    ])
    func aNameReachesJNIAsItTakesIt(name: String, sent: String) {
        #expect(HardwareKeyboard.deviceName(name) == sent)
    }

    @Test func aNameIsCutAtACharacterBoundaryWithinTheChannelsBytes() {
        let long = String(repeating: "é", count: 40)  // 80 bytes
        let cut = HardwareKeyboard.deviceName(long)
        #expect(cut.utf8.count <= 63)
        #expect(cut == String(repeating: "é", count: 31))
        #expect(HardwareKeyboard.deviceName(String(repeating: "日", count: 30)).utf8.count == 63)
    }
}
