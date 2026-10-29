/// A key of the host's hardware keyboard as the input channel takes it (`struct apkrt_key_event` in
/// guest_interface.h; ADR-0068 step 7c2).
public struct KeyRecord: Sendable, Equatable {
    /// AKEY_EVENT_ACTION_DOWN 0 or _UP 1.
    public var action: Int32
    public var keyCode: Int32
    public var metaState: Int32
    public var repeatCount: Int32
    /// The time of the key's DOWN, in ns on the guest's CLOCK_MONOTONIC.
    public var downTime: Int64
    public var eventTime: Int64
    public var deviceID: Int32
    /// AINPUT_SOURCE_KEYBOARD.
    public var source: Int32
}

/// The device the guest's input service lists for the keyboard (`struct apkrt_device_event`).
public struct KeyboardDevice: Sendable, Equatable {
    public var id: Int32
    public var name: String
    public var sources: Int32
}

/// One of UIKit's `UIPress`es of a hardware key: its HID usage (`UIKey.keyCode`) and its time in seconds on the uptime
/// clock (`UIPress.timestamp`), which is the guest's CLOCK_MONOTONIC.
public struct HardwareKeyPress: Sendable, Equatable {
    public var usage: Int
    public var timestamp: Double

    public init(usage: Int, timestamp: Double) {
        self.usage = usage
        self.timestamp = timestamp
    }
}

/// UIKit's press callbacks.
public enum KeyPhase: Sendable {
    case down, up, cancelled
}

public enum KeyboardEvent: Sendable, Equatable {
    case deviceAdded(KeyboardDevice)
    case key(KeyRecord)
}

/// UIKit's hardware key presses as Android's key events (ADR-0068 step 7c2), in the order of AOSP's
/// KeyboardInputMapper: the keyboard joins the guest's input devices with its first known key; a modifier's
/// bits are in the meta state of its own down and of the keys while it is held, an up carries the state after it
/// (KeyboardInputMapper::processKey updates the meta state before it sends the event); Caps Lock, Num Lock and
/// Scroll Lock toggle on their down; a down of a key already held is a repeat, counted from 1.
public struct HardwareKeyboard: Sendable {
    /// The host's device ID: above Android's own (-1) and the touchscreen's (1), as apk-run's device 1000.
    public static let deviceID: Int32 = 1000
    public static let name = "Hardware Keyboard"
    /// InputDevice.SOURCE_KEYBOARD.
    static let sourceKeyboard: Int32 = 0x101
    /// The longest name the input channel takes, bytes of UTF-8 below its NUL (`struct apkrt_device_event`).
    static let nameBytes = 63

    private struct Held {
        var usage: Int
        var downTime: Int64
        var repeats: Int32
    }

    private var added = false
    private var held: [Held] = []
    private var locks: Int32 = 0

    public init() {}

    /// `usage` going down or up at `time` (ns on the uptime clock); empty for a key without an Android code, an
    /// up without a down, and nothing else.
    public mutating func press(usage: Int, down: Bool, time: Int64) -> [KeyboardEvent] {
        guard let code = Self.androidKeyCode(forHIDUsage: usage) else { return [] }
        var events: [KeyboardEvent] = []
        if let index = held.firstIndex(where: { $0.usage == usage }) {
            guard down else {
                let key = held.remove(at: index)
                return [.key(record(code, action: 1, held: key, time: time))]
            }
            held[index].repeats += 1
            return [.key(record(code, action: 0, held: held[index], time: time))]
        }
        guard down else { return [] }
        if !added {
            added = true
            let device = KeyboardDevice(id: Self.deviceID, name: Self.name, sources: Self.sourceKeyboard)
            events.append(.deviceAdded(device))
        }
        locks ^= Self.lockBit(usage)
        held.append(Held(usage: usage, downTime: time, repeats: 0))
        events.append(.key(record(code, action: 0, held: held[held.count - 1], time: time)))
        return events
    }

    /// Ends every held key, oldest first, when the keyboard goes away or the session loses the keys (a fresh start of
    /// the state: no modifier stays held).
    public mutating func releaseAll(time: Int64) -> [KeyboardEvent] {
        var events: [KeyboardEvent] = []
        while !held.isEmpty {
            let key = held.removeFirst()
            guard let code = Self.androidKeyCode(forHIDUsage: key.usage) else { continue }
            events.append(.key(record(code, action: 1, held: key, time: time)))
        }
        return events
    }

    private func record(_ code: Int32, action: Int32, held key: Held, time: Int64) -> KeyRecord {
        KeyRecord(action: action, keyCode: code, metaState: metaState(), repeatCount: key.repeats,
                  downTime: key.downTime, eventTime: time, deviceID: Self.deviceID, source: Self.sourceKeyboard)
    }

    private func metaState() -> Int32 {
        held.reduce(locks) { $0 | Self.modifierBits($1.usage) }
    }

    /// META_*_ON of a modifier: its generic bit and its side's.
    static func modifierBits(_ usage: Int) -> Int32 {
        switch usage {
        case 0xe0: 0x1000 | 0x2000  // CTRL_ON | CTRL_LEFT_ON
        case 0xe1: 0x1 | 0x40  // SHIFT_ON | SHIFT_LEFT_ON
        case 0xe2: 0x2 | 0x10  // ALT_ON | ALT_LEFT_ON
        case 0xe3: 0x10000 | 0x20000  // META_ON | META_LEFT_ON
        case 0xe4: 0x1000 | 0x4000
        case 0xe5: 0x1 | 0x80
        case 0xe6: 0x2 | 0x20
        case 0xe7: 0x10000 | 0x40000
        default: 0
        }
    }

    /// META_CAPS_LOCK_ON, META_NUM_LOCK_ON and META_SCROLL_LOCK_ON for the lock keys.
    static func lockBit(_ usage: Int) -> Int32 {
        switch usage {
        case 0x39: 0x100000
        case 0x53: 0x200000
        case 0x47: 0x400000
        default: 0
        }
    }

    /// The name as `apkrt_input_post_device` takes it: Java's JNI wants UTF-8 with nothing above U+FFFF and no NUL
    /// inside, and the record has 63 bytes. A character outside the basic plane becomes U+FFFD, the name ends at a
    /// NUL, and it is cut at a character boundary.
    public static func deviceName(_ name: String) -> String {
        var result = ""
        var bytes = 0
        for scalar in name.unicodeScalars {
            if scalar == "\0" { break }
            let safe = scalar.value > 0xffff ? "\u{fffd}" : Character(scalar)
            let size = String(safe).utf8.count
            if bytes + size > nameBytes { break }
            result.append(safe)
            bytes += size
        }
        return result
    }

    /// UIKit's seconds as the ns of the guest's clock.
    public static func nanoseconds(_ seconds: Double) -> Int64 {
        Int64((seconds * 1e9).rounded())
    }

    /// Android's key code for a USB HID keyboard usage (usage page 7, UIKeyboardHIDUsage), as the Linux input
    /// layer and Generic.kl map the same keys; nil for a usage without one.
    public static func androidKeyCode(forHIDUsage usage: Int) -> Int32? {
        codes[usage]
    }

    private static let codes: [Int: Int32] = {
        var table: [Int: Int32] = [:]
        for index in 0..<26 { table[0x04 + index] = 29 + Int32(index) }  // KEYCODE_A to KEYCODE_Z
        for index in 0..<9 { table[0x1e + index] = 8 + Int32(index) }  // KEYCODE_1 to KEYCODE_9
        table[0x27] = 7  // KEYCODE_0
        let named: [(Int, Int32)] = [
            (0x28, 66), (0x29, 111), (0x2a, 67), (0x2b, 61), (0x2c, 62),  // ENTER ESCAPE DEL TAB SPACE
            (0x2d, 69), (0x2e, 70), (0x2f, 71), (0x30, 72), (0x31, 73),  // MINUS EQUALS [ ] BACKSLASH
            (0x32, 73), (0x33, 74), (0x34, 75), (0x35, 68), (0x36, 55),  // # ; ' GRAVE COMMA
            (0x37, 56), (0x38, 76), (0x39, 115),  // PERIOD SLASH CAPS_LOCK
            (0x46, 120), (0x47, 116), (0x48, 121), (0x49, 124), (0x4a, 122),  // SYSRQ SCROLL_LOCK BREAK INSERT HOME
            (0x4b, 92), (0x4c, 112), (0x4d, 123), (0x4e, 93),  // PAGE_UP FORWARD_DEL END PAGE_DOWN
            (0x4f, 22), (0x50, 21), (0x51, 20), (0x52, 19),  // DPAD_RIGHT LEFT DOWN UP
            (0x53, 143), (0x54, 154), (0x55, 155), (0x56, 156), (0x57, 157),  // NUM_LOCK and the keypad's / * - +
            (0x58, 160), (0x62, 144), (0x63, 158), (0x64, 73), (0x65, 82),  // NUMPAD_ENTER 0 DOT, BACKSLASH, MENU
            (0x67, 161),  // NUMPAD_EQUALS
            (0xe0, 113), (0xe1, 59), (0xe2, 57), (0xe3, 117),  // CTRL SHIFT ALT META, left
            (0xe4, 114), (0xe5, 60), (0xe6, 58), (0xe7, 118),  // and right
        ]
        for (usage, code) in named { table[usage] = code }
        for index in 0..<12 { table[0x3a + index] = 131 + Int32(index) }  // KEYCODE_F1 to F12
        for index in 0..<9 { table[0x59 + index] = 145 + Int32(index) }  // KEYCODE_NUMPAD_1 to 9
        return table
    }()
}
