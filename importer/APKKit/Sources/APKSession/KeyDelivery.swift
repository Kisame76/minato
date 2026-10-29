/// Where the hardware keyboard's records go: the guest's input channel, through the app's C calls (ADR-0068 step 7c2).
public protocol KeyPoster: AnyObject {
    /// Whether the channel took the device record (false for an unknown channel or a full one).
    func postDevice(channel: UInt64, _ device: KeyboardDevice) -> Bool
    func postKey(channel: UInt64, _ key: KeyRecord) -> Bool
}

/// Posts `HardwareKeyboard`'s events so that the guest lists the keyboard before it gets a key of it. The channel
/// exists only once the app has resumed, and a full one refuses a record: the device is kept and offered again before
/// each key until a channel has taken it, and keys wait for that (a key of a device the guest never listed makes
/// `KeyCharacterMap.load` throw). The guest ignores a device that it already has.
public final class KeyDelivery {
    /// The channel of the guest's input queue, 0 before it has one.
    public var channel: UInt64 = 0

    private weak var poster: KeyPoster?
    /// Key codes whose down was dropped: its repeats and up are dropped too, as an up alone has no down in the guest.
    private var dropped: Set<Int32> = []
    private var device: KeyboardDevice?
    private var takenBy: UInt64 = 0

    /// `poster` is not retained: its owner holds the delivery.
    public init(poster: KeyPoster) {
        self.poster = poster
    }

    public func post(_ events: [KeyboardEvent]) {
        for event in events {
            switch event {
            case .deviceAdded(let added):
                device = added
                takenBy = 0
                _ = offerDevice()
            case .key(let key):
                let first = key.action == 0 && key.repeatCount == 0
                guard offerDevice() && (first || !dropped.contains(key.keyCode)) else {
                    if first { dropped.insert(key.keyCode) }
                    continue
                }
                if first { dropped.remove(key.keyCode) }
                _ = poster?.postKey(channel: channel, key)
            }
        }
    }

    private func offerDevice() -> Bool {
        guard channel != 0, let device else { return false }
        if takenBy == channel { return true }
        if poster?.postDevice(channel: channel, device) == true { takenBy = channel }
        return takenBy == channel
    }
}
