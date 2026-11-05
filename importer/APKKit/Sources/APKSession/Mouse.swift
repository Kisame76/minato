/// A change of UIKit's pointer in the session view's points, with its time on the uptime clock in seconds.
public enum PointerChange: Sendable, Equatable {
    case moved(x: Double, y: Double, timestamp: Double)
    /// The buttons held now (UIEvent.buttonMask's bits), at `x`, `y`.
    case buttons(Int32, x: Double, y: Double, timestamp: Double)
    /// A scroll pan's translation since the last, in points.
    case scrolled(dx: Double, dy: Double, timestamp: Double)
    case exited(timestamp: Double)
}

/// UIKit's pointer, a mouse's or a trackpad's, as the reports apkrt_input_post_mouse takes (ADR-0068 decision 5, the
/// mouse step), from which the runtime makes Android 15's hover, button, key and scroll records. A move is a
/// HOVER_MOVE to the window's pixels, as touches are placed (TouchTranslator), with the move since the last report as
/// the relative axes; the first move after the pointer came or left has none, as UIKit gives positions only. Each
/// button that changes is a report of its own, releases before presses, lowest first, with the buttons held after it
/// (UIEvent.buttonMask's bits are Android's: primary 1, secondary 2, tertiary 4, back 8, forward 16), after a move to
/// where the pointer is. A scroll is in lines of 64 points, ViewConfiguration's scroll factor, so that an app scrolls
/// as far as UIKit would; positive up and left, as a wheel turned forward. The mouse joins the guest's devices with
/// its first report and leaves at its disconnection, with its buttons released.
public struct Mouse: Sendable {
    /// The host's device ID: the hardware keyboard's (1000) plus one; the pads come from 1010.
    public static let deviceID: Int32 = 1001
    /// AINPUT_SOURCE_MOUSE and AMOTION_EVENT_TOOL_TYPE_MOUSE.
    static let source: Int32 = 0x2002
    static let tool: Int32 = 3
    /// Points per line of a scroll (ViewConfiguration's 64 dp).
    static let pointsPerLine = 64.0

    public static let device = GamepadDevice(id: deviceID, name: "Mouse", sources: source, vendorID: 0, productID: 0)

    /// The window's pixels per point, and the guest's frame in size compat.
    public var scale: Double
    public var letterbox: Letterbox?

    private var added = false
    private var place: (x: Float, y: Float)?
    private var held: Int32 = 0

    public init(scale: Double) {
        self.scale = scale
    }

    /// The pointer is at `x`, `y` in the view's points.
    public mutating func moved(x: Double, y: Double, time: Int64) -> [PadEvent] {
        let (px, py) = pixels(x, y)
        guard place.map({ $0.x != px || $0.y != py }) ?? true else { return [] }
        var axes = [AxesRecord.Axis(axis: 0, value: px), AxesRecord.Axis(axis: 1, value: py)]
        if let place {
            if px != place.x { axes.append(AxesRecord.Axis(axis: 27, value: px - place.x)) }
            if py != place.y { axes.append(AxesRecord.Axis(axis: 28, value: py - place.y)) }
        }
        place = (px, py)
        return join() + [report(7, axes: axes, time: time)]
    }

    /// The buttons held now (UIEvent.buttonMask), with the pointer at `x`, `y`. The same buttons at another place are
    /// a drag's move: while a button is held only touchesMoved reports the pointer.
    public mutating func buttons(_ now: Int32, x: Double, y: Double, time: Int64) -> [PadEvent] {
        let wanted = now & 0x1f
        guard wanted != held else { return moved(x: x, y: y, time: time) }
        var events = moved(x: x, y: y, time: time)
        if events.isEmpty { events = join() }
        let position = place.map { [AxesRecord.Axis(axis: 0, value: $0.x), AxesRecord.Axis(axis: 1, value: $0.y)] }
        for bit in (0..<5).map({ Int32(1) << $0 }) where held & bit != 0 && wanted & bit == 0 {
            held &= ~bit
            events.append(report(12, axes: position ?? [], time: time))
        }
        for bit in (0..<5).map({ Int32(1) << $0 }) where held & bit == 0 && wanted & bit != 0 {
            held |= bit
            events.append(report(11, axes: position ?? [], time: time))
        }
        return events
    }

    /// A scroll of `dx`, `dy` points (UIPanGestureRecognizer's translation for a wheel or a trackpad's two fingers).
    public mutating func scrolled(dx: Double, dy: Double, time: Int64) -> [PadEvent] {
        var axes: [AxesRecord.Axis] = []
        if dy != 0 { axes.append(AxesRecord.Axis(axis: 9, value: Float(dy / Self.pointsPerLine))) }
        if dx != 0 { axes.append(AxesRecord.Axis(axis: 10, value: Float(dx / Self.pointsPerLine))) }
        guard !axes.isEmpty else { return [] }
        return join() + [report(8, axes: axes, time: time)]
    }

    /// The pointer left the view. UIKit's hover also ends at a click, which is no exit: the runtime makes Android's
    /// HOVER_EXIT of a press itself.
    public mutating func exited(time: Int64) -> [PadEvent] {
        guard place != nil, held == 0 else { return [] }
        place = nil
        return [report(10, axes: [], time: time)]
    }

    /// The mouse went (GCMouseDidDisconnect): its buttons are released, the pointer leaves, and the device goes.
    public mutating func disconnect(time: Int64) -> [PadEvent] {
        guard added else { return [] }
        var events: [PadEvent] = []
        if held != 0 {
            let position = place.map { [AxesRecord.Axis(axis: 0, value: $0.x), AxesRecord.Axis(axis: 1, value: $0.y)] }
            held = 0
            events.append(report(12, axes: position ?? [], time: time))
        }
        if place != nil { events.append(report(10, axes: [], time: time)) }
        place = nil
        added = false
        return events + [.deviceRemoved(Self.device)]
    }

    private mutating func join() -> [PadEvent] {
        guard !added else { return [] }
        added = true
        return [.deviceAdded(Self.device)]
    }

    private func report(_ action: Int32, axes: [AxesRecord.Axis], time: Int64) -> PadEvent {
        .axes(AxesRecord(action: action, eventTime: time, deviceID: Self.deviceID, source: Self.source, axes: axes,
                         buttonState: held, toolType: Self.tool))
    }

    private func pixels(_ x: Double, _ y: Double) -> (Float, Float) {
        let left = Double(letterbox?.frame.left ?? 0), top = Double(letterbox?.frame.top ?? 0)
        let guest = Double(letterbox?.scale ?? 1)
        return (Float((x * scale - left) / guest), Float((y * scale - top) / guest))
    }
}
