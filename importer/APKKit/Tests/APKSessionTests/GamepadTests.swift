import Testing

@testable import APKSession

/// ADR-0068 decision 5: a GCController's extended gamepad as the records apk-run's pad: steps post, which give
/// gamepad-test the lines Android 15 logs for a standard Xbox pad (fixtures/expected/gamepad-test.logcat).
@Suite struct GamepadTests {
    private func pad(_ category: String = "Xbox One", name: String? = "Xbox Wireless Controller") -> Gamepad {
        Gamepad(id: 1010, productCategory: category, vendorName: name)
    }

    private func keys(_ events: [PadEvent]) -> [String] {
        events.compactMap {
            guard case .key(let key) = $0 else { return nil }
            return "\(key.keyCode)/\(key.action) src \(String(key.source, radix: 16)) dev \(key.deviceID)"
                + " down \(key.downTime) at \(key.eventTime)"
        }
    }

    private func axes(_ events: [PadEvent]) -> [String] {
        events.compactMap {
            guard case .axes(let record) = $0 else { return nil }
            return record.axes.map { "\($0.axis)=\($0.value)" }.joined(separator: " ")
        }
    }

    @Test func aPadIsAnXboxLikeDeviceOfJoystickGamepadAndKeyboard() {
        let device = pad().device
        #expect(device.id == 1010)
        #expect(device.name == "Xbox Wireless Controller")
        #expect(device.sources == 0x0100_0511)
        #expect(device.vendorID == 0x45e && device.productID == 0x2e0)
    }

    @Test func theVendorAndProductFollowThePadsCategory() {
        #expect(pad("DualShock 4").device.vendorID == 0x54c && pad("DualShock 4").device.productID == 0x9cc)
        #expect(pad("DualSense").device.vendorID == 0x54c && pad("DualSense").device.productID == 0xce6)
        #expect(pad("Switch Pro Controller").device.vendorID == 0x57e
                && pad("Switch Pro Controller").device.productID == 0x2009)
        #expect(pad("MFi").device.vendorID == 0 && pad("MFi").device.productID == 0)
    }

    @Test func aPadWithoutANameIsAGameController() {
        #expect(pad(name: nil).device.name == "Game Controller")
        #expect(pad(name: "Pad \u{1F3AE}").device.name == "Pad \u{fffd}")
    }

    @Test func eachButtonIsItsAndroidKeyWithTheGamepadsSource() {
        var gamepad = pad()
        let order: [GamepadButton] = [.a, .b, .x, .y, .leftShoulder, .rightShoulder, .menu, .options, .home,
                                      .leftThumb, .rightThumb]
        var codes: [String] = []
        var time: Int64 = 100
        for button in order {
            var state = GamepadState()
            state.pressed = [button]
            codes += keys(gamepad.update(state, time: time))
            codes += keys(gamepad.update(GamepadState(), time: time + 5))
            time += 10
        }
        let expected = [96, 97, 99, 100, 102, 103, 108, 109, 110, 106, 107].enumerated().flatMap { index, code in
            let down = 100 + Int64(index) * 10
            return ["\(code)/0 src 501 dev 1010 down \(down) at \(down)",
                    "\(code)/1 src 501 dev 1010 down \(down) at \(down + 5)"]
        }
        #expect(codes == expected)
    }

    @Test func aHeldButtonGivesNoSecondDown() {
        var gamepad = pad()
        var state = GamepadState()
        state.pressed = [.a]
        #expect(keys(gamepad.update(state, time: 1)).count == 1)
        state.leftX = 0.5
        #expect(keys(gamepad.update(state, time: 2)).isEmpty)
    }

    @Test func theSticksAreXYAndZRZWithAndroidsUpNegative() {
        var gamepad = pad()
        var state = GamepadState()
        state.leftX = 0.5
        #expect(axes(gamepad.update(state, time: 1)) == ["0=0.5"])
        state = GamepadState()
        state.leftY = 1
        #expect(axes(gamepad.update(state, time: 2)) == ["1=-1.0"])
        state = GamepadState()
        state.rightX = 0.25
        #expect(axes(gamepad.update(state, time: 3)) == ["11=0.25"])
        state = GamepadState()
        state.rightY = 0.5
        #expect(axes(gamepad.update(state, time: 4)) == ["14=-0.5"])
    }

    @Test func theTriggersAreLTriggerAndRTriggerAndTheDpadIsTheHat() {
        var gamepad = pad()
        var state = GamepadState()
        state.leftTrigger = 0.5
        state.rightTrigger = 1
        #expect(axes(gamepad.update(state, time: 1)) == ["17=0.5 18=1.0"])
        state = GamepadState()
        state.dpadX = -1
        #expect(axes(gamepad.update(state, time: 2)) == ["15=-1.0"])
        state = GamepadState()
        state.dpadY = -1
        #expect(axes(gamepad.update(state, time: 3)) == ["16=1.0"])
    }

    @Test func aMoveCarriesEveryAxisThatIsNotZeroAndRestIsAnEmptyMove() {
        var gamepad = pad()
        var state = GamepadState()
        state.leftX = 1
        state.leftY = 0.5
        let events = gamepad.update(state, time: 7)
        #expect(axes(events) == ["0=1.0 1=-0.5"])
        guard case .axes(let record) = events.last else { Issue.record("no axes"); return }
        #expect(record.action == 2 && record.source == 0x0100_0010 && record.deviceID == 1010 && record.eventTime == 7)
        #expect(axes(gamepad.update(GamepadState(), time: 8)) == [""])
    }

    @Test func anUnchangedReportPostsNothing() {
        var gamepad = pad()
        var state = GamepadState()
        state.leftX = 0.5
        _ = gamepad.update(state, time: 1)
        #expect(gamepad.update(state, time: 2).isEmpty)
        var fresh = pad()
        #expect(fresh.update(GamepadState(), time: 1).isEmpty)
    }

    @Test func valuesAreClampedAndTheHatIsWhole() {
        var gamepad = pad()
        var state = GamepadState()
        state.leftX = 1.5
        state.leftTrigger = -0.2
        state.dpadX = 0.7
        state.rightX = .nan
        #expect(axes(gamepad.update(state, time: 1)) == ["0=1.0 15=1.0"])
    }

    @Test func keysComeBeforeTheMoveOfTheSameReport() {
        var gamepad = pad()
        var state = GamepadState()
        state.pressed = [.a]
        state.leftX = 0.5
        let events = gamepad.update(state, time: 1)
        guard events.count == 2, case .key = events[0], case .axes = events[1] else {
            Issue.record("\(events)")
            return
        }
    }

    @Test func aDisconnectReleasesTheHeldButtonsThenRemovesThePad() {
        var gamepad = pad()
        var state = GamepadState()
        state.pressed = [.b, .a]
        state.leftX = 0.5
        _ = gamepad.update(state, time: 1)
        let events = gamepad.disconnect(time: 9)
        #expect(keys(events) == ["96/1 src 501 dev 1010 down 1 at 9", "97/1 src 501 dev 1010 down 1 at 9"])
        guard case .deviceRemoved(let device) = events.last else { Issue.record("\(events)"); return }
        #expect(device.id == 1010)
        #expect(axes(events).isEmpty)
    }
}

/// The pads' records reach a channel only once it has listed their device, as KeyDelivery does for the keyboard.
@Suite struct PadDeliveryTests {
    final class Poster: PadPoster {
        var calls: [String] = []
        var takesDevices = true

        func postDevice(channel: UInt64, _ device: GamepadDevice, added: Bool) -> Bool {
            calls.append("\(added ? "add" : "remove") \(device.id) on \(channel)")
            return takesDevices
        }

        func postKey(channel: UInt64, _ key: KeyRecord) -> Bool {
            calls.append("key \(key.keyCode)/\(key.action) on \(channel)")
            return true
        }

        func postAxes(channel: UInt64, _ axes: AxesRecord) -> Bool {
            calls.append("axes \(axes.deviceID) on \(channel)")
            return true
        }
    }

    private let device = Gamepad(id: 1010, productCategory: "Xbox One", vendorName: "Pad").device
    private let other = Gamepad(id: 1011, productCategory: "DualSense", vendorName: "Pad 2").device

    private func key(_ code: Int32, _ action: Int32, device id: Int32 = 1010) -> PadEvent {
        .key(KeyRecord(action: action, keyCode: code, metaState: 0, repeatCount: 0, downTime: 1, eventTime: 1,
                       deviceID: id, source: 0x501))
    }

    private func move(device id: Int32 = 1010) -> PadEvent {
        .axes(AxesRecord(action: 2, eventTime: 1, deviceID: id, source: 0x0100_0010, axes: []))
    }

    @Test func theDeviceGoesBeforeItsFirstRecord() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.channel = 7
        delivery.post([.deviceAdded(device), key(96, 0), move()])
        #expect(poster.calls == ["add 1010 on 7", "key 96/0 on 7", "axes 1010 on 7"])
    }

    @Test func withoutAChannelThePadIsKeptAndOfferedWithItsNextRecord() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.post([.deviceAdded(device), key(96, 0), key(96, 1)])
        #expect(poster.calls.isEmpty)
        delivery.channel = 7
        delivery.post([move()])
        #expect(poster.calls == ["add 1010 on 7", "axes 1010 on 7"])
    }

    @Test func aRefusedDeviceIsOfferedAgainAndItsRecordsWait() {
        let poster = Poster()
        poster.takesDevices = false
        let delivery = PadDelivery(poster: poster)
        delivery.channel = 7
        delivery.post([.deviceAdded(device), key(96, 0)])
        poster.takesDevices = true
        delivery.post([key(96, 1), key(97, 0)])
        #expect(poster.calls == ["add 1010 on 7", "add 1010 on 7", "add 1010 on 7", "key 97/0 on 7"])
    }

    @Test func anUpWhoseDownWasDroppedIsDroppedToo() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.post([.deviceAdded(device), key(96, 0)])
        delivery.channel = 7
        delivery.post([key(96, 1), key(96, 0), key(96, 1)])
        #expect(poster.calls == ["add 1010 on 7", "key 96/0 on 7", "key 96/1 on 7"])
    }

    @Test func aRemovalGoesOnlyToAChannelThatListedThePad() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.post([.deviceAdded(device), .deviceRemoved(device)])
        delivery.channel = 7
        delivery.post([move()])
        #expect(poster.calls.isEmpty)
        delivery.post([.deviceAdded(device), move(), .deviceRemoved(device), move()])
        #expect(poster.calls == ["add 1010 on 7", "axes 1010 on 7", "remove 1010 on 7"])
    }

    @Test func aChannelThatRefusedThePadHearsNoRemoval() {
        let poster = Poster()
        poster.takesDevices = false
        let delivery = PadDelivery(poster: poster)
        delivery.channel = 7
        delivery.post([.deviceAdded(device), .deviceRemoved(device)])
        #expect(poster.calls == ["add 1010 on 7"])
    }

    @Test func twoPadsAreListedEachOnItsOwn() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.channel = 7
        delivery.post([.deviceAdded(device), .deviceAdded(other), move(device: 1011), move()])
        #expect(poster.calls == ["add 1010 on 7", "add 1011 on 7", "axes 1011 on 7", "axes 1010 on 7"])
    }

    @Test func aNewChannelHearsOfThePadsAgain() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.channel = 7
        delivery.post([.deviceAdded(device)])
        delivery.channel = 8
        delivery.post([move()])
        #expect(poster.calls == ["add 1010 on 7", "add 1010 on 8", "axes 1010 on 8"])
    }

    @Test func aChannelThatComesListsThePadsThatAreThere() {
        let poster = Poster()
        let delivery = PadDelivery(poster: poster)
        delivery.post([.deviceAdded(device), .deviceAdded(other), .deviceRemoved(other)])
        delivery.offerAll()
        #expect(poster.calls.isEmpty)
        delivery.channel = 7
        delivery.offerAll()
        delivery.offerAll()
        #expect(poster.calls == ["add 1010 on 7"])
    }
}
