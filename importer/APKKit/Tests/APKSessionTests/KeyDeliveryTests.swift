import Testing

@testable import APKSession

/// ADR-0068 step 7c2: the keyboard joins the guest's input devices before its first key arrives there, though the
/// channel may be missing or full when the first key is pressed.
@Suite struct KeyDeliveryTests {
    final class Poster: KeyPoster {
        var calls: [String] = []
        var takesDevices = true
        var takesKeys = true

        func postDevice(channel: UInt64, _ device: KeyboardDevice) -> Bool {
            calls.append("device \(device.id) on \(channel)")
            return takesDevices
        }

        func postKey(channel: UInt64, _ key: KeyRecord) -> Bool {
            calls.append("key \(key.keyCode)/\(key.action) on \(channel)")
            return takesKeys
        }
    }

    private func firstKey() -> [KeyboardEvent] {
        var keyboard = HardwareKeyboard()
        return keyboard.press(usage: 0x04, down: true, time: 1)
    }

    @Test func theDeviceGoesBeforeTheFirstKey() {
        let poster = Poster()
        let delivery = KeyDelivery(poster: poster)
        delivery.channel = 7
        delivery.post(firstKey())

        #expect(poster.calls == ["device 1000 on 7", "key 29/0 on 7"])
    }

    @Test func keysWithoutAChannelAreDroppedAndTheDeviceComesWithTheFirstKeyAfterIt() {
        let poster = Poster()
        let delivery = KeyDelivery(poster: poster)
        delivery.post(firstKey())  // the app is still loading: no channel
        #expect(poster.calls.isEmpty)

        delivery.channel = 7
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: 0x04, down: true, time: 1)
        delivery.post(keyboard.press(usage: 0x05, down: true, time: 2))

        #expect(poster.calls == ["device 1000 on 7", "key 30/0 on 7"])
    }

    @Test func aDeviceTheChannelRefusedIsOfferedAgainBeforeTheNextKeyAndKeysWaitForIt() {
        let poster = Poster()
        poster.takesDevices = false
        let delivery = KeyDelivery(poster: poster)
        delivery.channel = 7
        delivery.post(firstKey())
        // The device is offered with its event and again for the key; no key goes before it is listed.
        #expect(poster.calls == ["device 1000 on 7", "device 1000 on 7"])

        poster.takesDevices = true
        var keyboard = HardwareKeyboard()
        _ = keyboard.press(usage: 0x04, down: true, time: 1)
        delivery.post(keyboard.press(usage: 0x04, down: false, time: 2))

        #expect(poster.calls == ["device 1000 on 7", "device 1000 on 7", "device 1000 on 7", "key 29/1 on 7"])
    }

    @Test func aTakenDeviceIsNotOfferedAgain() {
        let poster = Poster()
        let delivery = KeyDelivery(poster: poster)
        delivery.channel = 7
        var keyboard = HardwareKeyboard()
        delivery.post(keyboard.press(usage: 0x04, down: true, time: 1))
        delivery.post(keyboard.press(usage: 0x04, down: false, time: 2))

        #expect(poster.calls == ["device 1000 on 7", "key 29/0 on 7", "key 29/1 on 7"])
    }

    @Test func aNewChannelGetsTheDeviceAgain() {
        let poster = Poster()
        let delivery = KeyDelivery(poster: poster)
        delivery.channel = 7
        var keyboard = HardwareKeyboard()
        delivery.post(keyboard.press(usage: 0x04, down: true, time: 1))
        delivery.channel = 9
        delivery.post(keyboard.press(usage: 0x04, down: false, time: 2))

        #expect(poster.calls.suffix(2) == ["device 1000 on 9", "key 29/1 on 9"])
    }
}
