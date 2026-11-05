/// A game controller's buttons as Android 15 gives a standard Xbox pad's (Generic.kl's BTN_* lines; ADR-0068 decision
/// 5), in the order a report's keys go out. The triggers are axes only, as on Android, and the d-pad is the hat.
public enum GamepadButton: Sendable, CaseIterable {
    case a, b, x, y, leftShoulder, rightShoulder, leftThumb, rightThumb, menu, options, home

    /// KEYCODE_BUTTON_*.
    var keyCode: Int32 {
        switch self {
        case .a: 96
        case .b: 97
        case .x: 99
        case .y: 100
        case .leftShoulder: 102  // L1
        case .rightShoulder: 103  // R1
        case .leftThumb: 106  // THUMBL
        case .rightThumb: 107  // THUMBR
        case .menu: 108  // START
        case .options: 109  // SELECT
        case .home: 110  // MODE
        }
    }
}

/// One report of a GCExtendedGamepad in GameController's terms: the pressed buttons, the sticks and the d-pad from -1
/// to 1 with up positive, the triggers from 0 to 1.
public struct GamepadState: Sendable, Equatable {
    public var pressed: Set<GamepadButton> = []
    public var leftX: Float = 0
    public var leftY: Float = 0
    public var rightX: Float = 0
    public var rightY: Float = 0
    public var leftTrigger: Float = 0
    public var rightTrigger: Float = 0
    public var dpadX: Float = 0
    public var dpadY: Float = 0

    public init() {}
}

/// The device the guest's input service lists for a pad (`struct apkrt_device_event`).
public struct GamepadDevice: Sendable, Equatable {
    public var id: Int32
    public var name: String
    public var sources: Int32
    public var vendorID: Int32
    public var productID: Int32
}

/// A pad's motion as the input channel takes it (`struct apkrt_axes_event`): the axes that are not zero, as
/// AMOTION_EVENT_AXIS_* and their values.
public struct AxesRecord: Sendable, Equatable {
    public struct Axis: Sendable, Equatable {
        public var axis: Int32
        public var value: Float
    }

    /// AMOTION_EVENT_ACTION_MOVE 2 for a pad.
    public var action: Int32
    /// ns on the guest's CLOCK_MONOTONIC.
    public var eventTime: Int64
    public var deviceID: Int32
    public var source: Int32
    public var axes: [Axis]
    /// A mouse's: AMOTION_EVENT_BUTTON_* held after the report, and the tool (MOUSE 3); a pad's are 0.
    public var buttonState: Int32 = 0
    public var toolType: Int32 = 0
}

public enum PadEvent: Sendable, Equatable {
    case deviceAdded(GamepadDevice)
    case deviceRemoved(GamepadDevice)
    case key(KeyRecord)
    case axes(AxesRecord)
}

/// A connected controller as Android 15 hands a standard pad to an app (ADR-0068 decision 5): one input device of
/// SOURCE_JOYSTICK | SOURCE_GAMEPAD | SOURCE_KEYBOARD; a button's change is a key of SOURCE_GAMEPAD | SOURCE_KEYBOARD
/// (KeyboardInputMapper), and a change of any axis one ACTION_MOVE of SOURCE_JOYSTICK with every axis that is not zero
/// (JoystickInputMapper): the left stick X and Y, the right stick Z and RZ, the triggers LTRIGGER and RTRIGGER (the
/// guest adds BRAKE and GAS), the d-pad HAT_X and HAT_Y, with up negative. A report's keys come before its move, as the
/// keyboard mapper comes before the joystick mapper in InputDevice::populateMappers.
public struct Gamepad: Sendable {
    /// InputDevice.SOURCE_JOYSTICK | SOURCE_GAMEPAD | SOURCE_KEYBOARD, as Android 15 lists an Xbox pad.
    static let sources: Int32 = 0x0100_0511
    static let keySource: Int32 = 0x501
    static let motionSource: Int32 = 0x0100_0010

    public let device: GamepadDevice
    private var downTimes: [GamepadButton: Int64] = [:]
    private var lastAxes: [AxesRecord.Axis] = []

    /// `productCategory` is GCDevice's (GCProductCategory*), which gives the vendor and product IDs games read to
    /// choose a layout: the Xbox Wireless Controller's, the DualShock 4's, the DualSense's, the Switch Pro
    /// Controller's, else none (0), as GameController names no USB IDs.
    public init(id: Int32, productCategory: String, vendorName: String?) {
        let ids: (Int32, Int32) = switch productCategory {
        case "Xbox One": (0x45e, 0x2e0)
        case "DualShock 4": (0x54c, 0x9cc)
        case "DualSense": (0x54c, 0xce6)
        case "Switch Pro Controller": (0x57e, 0x2009)
        default: (0, 0)
        }
        let name = HardwareKeyboard.deviceName(vendorName ?? "")
        device = GamepadDevice(id: id, name: name.isEmpty ? "Game Controller" : name, sources: Self.sources,
                               vendorID: ids.0, productID: ids.1)
    }

    /// The records of a report at `time` (ns on the uptime clock): a key for each button that went down or up, then a
    /// move when an axis changed.
    public mutating func update(_ state: GamepadState, time: Int64) -> [PadEvent] {
        var events: [PadEvent] = []
        for button in GamepadButton.allCases {
            let down = state.pressed.contains(button)
            if down, downTimes[button] == nil {
                downTimes[button] = time
                events.append(.key(key(button, action: 0, down: time, time: time)))
            } else if !down, let downTime = downTimes.removeValue(forKey: button) {
                events.append(.key(key(button, action: 1, down: downTime, time: time)))
            }
        }
        let axes = Self.axes(of: state)
        if axes != lastAxes {
            lastAxes = axes
            events.append(.axes(AxesRecord(action: 2, eventTime: time, deviceID: device.id, source: Self.motionSource,
                                           axes: axes)))
        }
        return events
    }

    /// The pad went: the ups of the buttons held, then its removal.
    public mutating func disconnect(time: Int64) -> [PadEvent] {
        var events: [PadEvent] = []
        for button in GamepadButton.allCases {
            if let downTime = downTimes.removeValue(forKey: button) {
                events.append(.key(key(button, action: 1, down: downTime, time: time)))
            }
        }
        lastAxes = []
        events.append(.deviceRemoved(device))
        return events
    }

    private func key(_ button: GamepadButton, action: Int32, down: Int64, time: Int64) -> KeyRecord {
        KeyRecord(action: action, keyCode: button.keyCode, metaState: 0, repeatCount: 0, downTime: down,
                  eventTime: time, deviceID: device.id, source: Self.keySource)
    }

    /// The axes that are not zero, in Android's numbering: X 0, Y 1, Z 11, RZ 14, HAT_X 15, HAT_Y 16, LTRIGGER 17,
    /// RTRIGGER 18. A stick is clamped to -1 to 1, a trigger to 0 to 1, the hat is -1, 0 or 1; a value that is not a
    /// number is 0.
    static func axes(of state: GamepadState) -> [AxesRecord.Axis] {
        func stick(_ value: Float) -> Float { value.isFinite ? min(1, max(-1, value)) : 0 }
        func trigger(_ value: Float) -> Float { value.isFinite ? min(1, max(0, value)) : 0 }
        func hat(_ value: Float) -> Float { value > 0.5 ? 1 : value < -0.5 ? -1 : 0 }
        let values: [(Int32, Float)] = [
            (0, stick(state.leftX)), (1, -stick(state.leftY)), (11, stick(state.rightX)), (14, -stick(state.rightY)),
            (15, hat(state.dpadX)), (16, -hat(state.dpadY)), (17, trigger(state.leftTrigger)),
            (18, trigger(state.rightTrigger)),
        ]
        return values.filter { $0.1 != 0 }.map { AxesRecord.Axis(axis: $0.0, value: $0.1) }
    }
}

/// Where the pads' records go: the guest's input channel, through the app's C calls.
public protocol PadPoster: AnyObject {
    /// Whether the channel took the device record of the pad's coming (`added`) or going.
    func postDevice(channel: UInt64, _ device: GamepadDevice, added: Bool) -> Bool
    func postKey(channel: UInt64, _ key: KeyRecord) -> Bool
    func postAxes(channel: UInt64, _ axes: AxesRecord) -> Bool
}

/// Posts the pads' events so that the guest lists a pad before it gets a record of it, as KeyDelivery does for the
/// keyboard: a pad is kept and offered again before each of its records until a channel has taken it, and its records
/// wait for that (dropped); an up whose down was dropped is dropped too. A pad's removal goes only to the channel that
/// listed it.
public final class PadDelivery {
    /// The channel of the guest's input queue, 0 before it has one.
    public var channel: UInt64 = 0

    private weak var poster: PadPoster?
    private var devices: [Int32: GamepadDevice] = [:]
    private var takenBy: [Int32: UInt64] = [:]
    private var dropped: Set<[Int32]> = []

    /// `poster` is not retained: its owner holds the delivery.
    public init(poster: PadPoster) {
        self.poster = poster
    }

    public func post(_ events: [PadEvent]) {
        for event in events {
            switch event {
            case .deviceAdded(let device):
                devices[device.id] = device
                takenBy[device.id] = nil
                _ = offer(device.id)
            case .deviceRemoved(let device):
                devices[device.id] = nil
                dropped = dropped.filter { $0[0] != device.id }
                if channel != 0, takenBy.removeValue(forKey: device.id) == channel {
                    _ = poster?.postDevice(channel: channel, device, added: false)
                }
            case .key(let key):
                let held = [key.deviceID, key.keyCode]
                guard offer(key.deviceID), key.action == 0 || !dropped.contains(held) else {
                    if key.action == 0 { dropped.insert(held) }
                    continue
                }
                if key.action == 0 { dropped.remove(held) }
                _ = poster?.postKey(channel: channel, key)
            case .axes(let axes):
                guard offer(axes.deviceID) else { continue }
                _ = poster?.postAxes(channel: channel, axes)
            }
        }
    }

    /// Offers every pad that is there to the channel, when it comes: the app lists the pads connected before its start
    /// without a report of theirs first, as Android lists them.
    public func offerAll() {
        for id in devices.keys.sorted() { _ = offer(id) }
    }

    private func offer(_ id: Int32) -> Bool {
        guard channel != 0, let device = devices[id] else { return false }
        if takenBy[id] == channel { return true }
        if poster?.postDevice(channel: channel, device, added: true) == true { takenBy[id] = channel }
        return takenBy[id] == channel
    }
}
