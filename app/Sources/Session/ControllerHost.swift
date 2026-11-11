import APKSession
import Foundation
import GameController

/// The game controllers for the guest (ADR-0068 decision 5): each GCController with an extended gamepad becomes a
/// `Gamepad`, from its connection, or from the session's start for one already connected, to its disconnection; its
/// reports come on the main queue (GCController's handler queue by default) and go out as records. A pad with haptics
/// rumbles as Android 15's FF_RUMBLE pad does (PadRumbleHost), through its own CoreHaptics engines.
@MainActor
final class ControllerHost {
    /// The first pad's device ID: above the hardware keyboard's (1000) and the mouse's.
    static let firstID: Int32 = 1010

    private let post: ([PadEvent]) -> Void
    private var pads: [ObjectIdentifier: Gamepad] = [:]
    private var nextID = firstID
    private var observers: [any NSObjectProtocol] = []
    private var controllers: [Int32: GCController] = [:]
    private lazy var rumble = PadRumbleHost { [weak self] id in
        #if DEBUG
        if Self.fakeHaptics { return LoggingHaptics(id: id) }
        #endif
        return self?.controllers[id].flatMap { ControllerHaptics($0) }
    }

    #if DEBUG
    /// SessionCheck's pad-rumble: a pad that connects gets a fake that logs its motors' commands, as the Simulator's
    /// virtual pad has no haptics.
    static var fakeHaptics = false

    /// The guest's device ID of a connected pad.
    func deviceID(of controller: GCController) -> Int32? {
        pads[ObjectIdentifier(controller)]?.device.id
    }
    #endif
    private var rumbleTimer: Task<Void, Never>?

    init(post: @escaping ([PadEvent]) -> Void) {
        self.post = post
    }

    func start() {
        guard observers.isEmpty else { return }
        let center = NotificationCenter.default
        observers = [
            center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
                // Posted on the main queue, where GameController calls its handlers too.
                nonisolated(unsafe) let controller = note.object as? GCController
                MainActor.assumeIsolated { if let controller { self?.connect(controller) } }
            },
            center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
                // Posted on the main queue, where GameController calls its handlers too.
                nonisolated(unsafe) let controller = note.object as? GCController
                MainActor.assumeIsolated { if let controller { self?.disconnect(controller) } }
            },
        ]
        for controller in GCController.controllers() { connect(controller) }
    }

    /// The session ends: the handlers go and every pad leaves the guest.
    func stop() {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers = []
        for controller in GCController.controllers() { disconnect(controller) }
        pads = [:]
        rumble.end()
        controllers = [:]
        schedule(nil)
    }

    /// The vibrators of a pad's InputDevice: Android's two FF_RUMBLE ones (0 strong, 1 weak) for a pad with haptics,
    /// none for one without.
    func vibratorIDs(of deviceID: Int32) -> [Int32] {
        rumble.vibratorIDs(deviceID)
    }

    func isVibrating(_ deviceID: Int32) -> Bool {
        rumble.isVibrating(deviceID)
    }

    func vibrate(_ deviceID: Int32, _ vibration: PadVibration) {
        schedule(rumble.vibrate(deviceID, vibration, now: Self.now))
    }

    func cancelVibration(_ deviceID: Int32, token: Int32) {
        schedule(rumble.cancel(deviceID, token: token))
    }

    /// The app went to the background: every pad stops rumbling.
    func enteredBackground() {
        rumble.stopAll()
        schedule(nil)
    }

    /// The timer for the next step of the pads' vibrations, at `deadline` on the uptime clock (ns), or none.
    private func schedule(_ deadline: Int64?) {
        rumbleTimer?.cancel()
        rumbleTimer = nil
        guard let deadline else { return }
        rumbleTimer = Task { @MainActor [weak self] in
            let wait = max(deadline - Self.now, 0)
            try? await Task.sleep(nanoseconds: UInt64(wait))
            guard !Task.isCancelled, let self else { return }
            self.schedule(self.rumble.advance(now: max(Self.now, deadline)))
        }
    }

    private func connect(_ controller: GCController) {
        let key = ObjectIdentifier(controller)
        guard pads[key] == nil, let gamepad = controller.extendedGamepad else { return }
        #if DEBUG
        if SessionCheck.ignoresController { return SessionCheck.event("pad ignored \(controller.productCategory)") }
        #endif
        let pad = Gamepad(id: nextID, productCategory: controller.productCategory, vendorName: controller.vendorName)
        nextID += 1
        pads[key] = pad
        controllers[pad.device.id] = controller
        var hasHaptics = controller.haptics != nil
        #if DEBUG
        hasHaptics = hasHaptics || Self.fakeHaptics
        #endif
        rumble.add(pad.device.id, hasHaptics: hasHaptics)
        #if DEBUG
        SessionCheck.event("pad connected \(pad.device.id) \(controller.productCategory)")
        #endif
        post([.deviceAdded(pad.device)])
        gamepad.valueChangedHandler = { [weak self, weak controller] gamepad, _ in
            MainActor.assumeIsolated {
                guard let self, let controller else { return }
                self.changed(controller, Self.state(of: gamepad))
            }
        }
        changed(controller, Self.state(of: gamepad))
    }

    private func disconnect(_ controller: GCController) {
        guard var pad = pads.removeValue(forKey: ObjectIdentifier(controller)) else { return }
        controller.extendedGamepad?.valueChangedHandler = nil
        rumble.remove(pad.device.id)
        controllers[pad.device.id] = nil
        #if DEBUG
        SessionCheck.event("pad disconnected \(pad.device.id)")
        #endif
        post(pad.disconnect(time: Self.now))
    }

    private func changed(_ controller: GCController, _ state: GamepadState) {
        let key = ObjectIdentifier(controller)
        guard var pad = pads[key] else { return }
        let events = pad.update(state, time: Self.now)
        pads[key] = pad
        if !events.isEmpty { post(events) }
    }

    /// The uptime clock, the guest's CLOCK_MONOTONIC, in ns.
    private static var now: Int64 {
        HardwareKeyboard.nanoseconds(ProcessInfo.processInfo.systemUptime)
    }

    private static func state(of gamepad: GCExtendedGamepad) -> GamepadState {
        var state = GamepadState()
        let buttons: [(GamepadButton, GCControllerButtonInput?)] = [
            (.a, gamepad.buttonA), (.b, gamepad.buttonB), (.x, gamepad.buttonX), (.y, gamepad.buttonY),
            (.leftShoulder, gamepad.leftShoulder), (.rightShoulder, gamepad.rightShoulder),
            (.leftThumb, gamepad.leftThumbstickButton), (.rightThumb, gamepad.rightThumbstickButton),
            (.menu, gamepad.buttonMenu), (.options, gamepad.buttonOptions), (.home, gamepad.buttonHome),
        ]
        state.pressed = Set(buttons.compactMap { $0.1?.isPressed == true ? $0.0 : nil })
        state.leftX = gamepad.leftThumbstick.xAxis.value
        state.leftY = gamepad.leftThumbstick.yAxis.value
        state.rightX = gamepad.rightThumbstick.xAxis.value
        state.rightY = gamepad.rightThumbstick.yAxis.value
        state.leftTrigger = gamepad.leftTrigger.value
        state.rightTrigger = gamepad.rightTrigger.value
        state.dpadX = gamepad.dpad.xAxis.value
        state.dpadY = gamepad.dpad.yAxis.value
        return state
    }
}
