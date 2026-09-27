/// One tick of the display link (ADR-0036, decision 2), in ns on the guest's CLOCK_MONOTONIC: the display
/// link's `timestamp` and `targetTimestamp` count on the uptime clock (F10).
public struct FrameTick: Sendable, Equatable {
    /// The vsync's time: Choreographer's frame time.
    public var time: Int64
    /// When the next frame is due.
    public var targetTime: Int64

    /// The frame period.
    public var period: Int64 { targetTime - time }

    /// From the display link's times, in seconds.
    public init(timestamp: Double, targetTimestamp: Double) {
        time = Int64((timestamp * 1e9).rounded())
        targetTime = Int64((targetTimestamp * 1e9).rounded())
    }
}

/// Where the ticks go: the vsync channel (ADR-0036, decision 1). The app implements it. It is called on
/// the display link's own thread, never the main thread, and must not block.
public protocol FrameTickSink: AnyObject, Sendable {
    func tick(_ tick: FrameTick)
}

/// A display link's frame rate range in Hz (`CAFrameRateRange`).
public struct FrameRateRange: Sendable, Equatable {
    public var minimum: Float
    public var maximum: Float
    public var preferred: Float

    public init(minimum: Float, maximum: Float, preferred: Float) {
        self.minimum = minimum
        self.maximum = maximum
        self.preferred = preferred
    }
}

public enum FramePacing {
    public static let minimumRate: Float = 60
    public static let maximumRate: Float = 120

    /// The range for the rate `ANativeWindow_setFrameRate` prefers, 0 for none: 60 to 120 Hz, preferring
    /// the rate clamped into that range, or 120 Hz without one.
    public static func range(preferred rate: Float) -> FrameRateRange {
        let preferred = rate.isFinite && rate > 0 ? min(max(rate, minimumRate), maximumRate) : maximumRate
        return FrameRateRange(minimum: minimumRate, maximum: maximumRate, preferred: preferred)
    }
}
