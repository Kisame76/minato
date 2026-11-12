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
    /// The global frame rate limit's steps (Settings > Apps > Frame Rate Limit): divisors of 120 only, as ADR-0066's.
    public static let limits = [120, 60, 30]

    /// The range for the rate `ANativeWindow_setFrameRate` prefers, 0 for none: 60 to 120 Hz, preferring
    /// the rate clamped into that range, or 120 Hz without one. With the app's FPS unlock (ADR-0066 decision 3), 0 for
    /// none, it prefers at least the unlock, so that a guest asking a lower rate cannot hold the link below it. The
    /// global limit, 0 for none, is the range's maximum (and its minimum where it is lower than 60); an unlock above
    /// it wins, as the user's choice for that app.
    public static func range(preferred rate: Float, unlock: Float = 0, limit: Float = 0) -> FrameRateRange {
        let maximum = limit > 0 ? min(max(limit, unlock), maximumRate) : maximumRate
        let minimum = min(minimumRate, maximum)
        let asked = rate.isFinite && rate > 0 ? rate : maximumRate
        let preferred = min(max(asked, minimum), maximum)
        return FrameRateRange(minimum: minimum, maximum: maximum, preferred: max(preferred, min(unlock, maximum)))
    }

    /// The display's refresh rate the guest sees (its configuration, Display.getRefreshRate): the screen's highest, at
    /// most the limit, or the unlock where that is higher, as Android's apps see 60 with Smooth Display off.
    public static func displayRate(screenMaximum: Float, limit: Float, unlock: Float) -> Float {
        limit > 0 ? min(screenMaximum, max(limit, unlock)) : screenMaximum
    }

    /// The rate graphics/host holds an app's presents to (ADR-0078 decision 4, minato_graphics_set_frame_rate_limit),
    /// for apps that present without pacing: the limit, 0 for none, or none where the unlock is at or above it
    /// (decision 2).
    public static func presentLimit(limit: Float, unlock: Float) -> Int {
        limit > 0 && unlock < limit ? Int(limit) : 0
    }

    /// The vsync's period in ns while the scene is in the background (apkrt_vsync_background): Android's 60 Hz
    /// without a display, or the guest's display rate where the frame rate limit makes it lower.
    public static func backgroundVsyncPeriod(displayRate: Float) -> Int64 {
        let rate = displayRate.isFinite && displayRate > 0 ? min(displayRate, minimumRate) : minimumRate
        return Int64((1e9 / Double(rate)).rounded(.up))
    }

    /// The steps Settings offers: those the screen shows (UIScreen.maximumFramesPerSecond), at least 30.
    public static func limitSteps(screenMaximum: Int) -> [Int] {
        let shown = limits.filter { $0 <= screenMaximum }
        return shown.isEmpty ? [limits.last!] : shown
    }

    /// The limit of the stored choice, 0 for none: only a step below the screen's highest limits anything.
    public static func limit(stored: Int, screenMaximum: Int) -> Int {
        limits.contains(stored) && stored < screenMaximum ? stored : 0
    }
}
