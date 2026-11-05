import APKSession
import Foundation
import GameController

/// The game controllers for the guest (ADR-0068 decision 5): each GCController with an extended gamepad becomes a
/// `Gamepad`, from its connection, or from the session's start for one already connected, to its disconnection; its
/// reports come on the main queue (GCController's handler queue by default) and go out as records.
@MainActor
final class ControllerHost {
    /// The first pad's device ID: above the hardware keyboard's (1000) and the mouse's.
    static let firstID: Int32 = 1010

    private let post: ([PadEvent]) -> Void
    private var pads: [ObjectIdentifier: Gamepad] = [:]
    private var nextID = firstID
    private var observers: [any NSObjectProtocol] = []

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
