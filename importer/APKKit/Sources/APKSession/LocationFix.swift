import Foundation

/// A CLLocation's values, as CoreLocation reports them: a negative accuracy, speed or course is its "invalid".
public struct LocationReading: Sendable, Equatable {
    public var latitude: Double
    public var longitude: Double
    public var altitude: Double
    public var horizontalAccuracy: Double
    public var verticalAccuracy: Double
    public var speed: Double
    public var speedAccuracy: Double
    public var course: Double
    public var courseAccuracy: Double
    public var timestamp: Date

    public init(latitude: Double, longitude: Double, altitude: Double, horizontalAccuracy: Double,
                verticalAccuracy: Double, speed: Double, speedAccuracy: Double, course: Double, courseAccuracy: Double,
                timestamp: Date) {
        self.latitude = latitude
        self.longitude = longitude
        self.altitude = altitude
        self.horizontalAccuracy = horizontalAccuracy
        self.verticalAccuracy = verticalAccuracy
        self.speed = speed
        self.speedAccuracy = speedAccuracy
        self.course = course
        self.courseAccuracy = courseAccuracy
        self.timestamp = timestamp
    }
}

/// `struct apkrt_location_fix` (guest_interface.h; ADR-0063 decision 3) of a reading: each field CoreLocation marks
/// invalid is 0 with its APKRT_LOCATION_HAS_* bit clear (an altitude counts only with a vertical accuracy, as
/// CLLocation's altitude is invalid without one), and elapsedRealtimeNanos is the reading's age taken from the guest's
/// CLOCK_BOOTTIME now, between 0 and now.
public struct LocationFixValues: Sendable, Equatable {
    static let hasAltitude: UInt32 = 1
    static let hasHorizontalAccuracy: UInt32 = 2
    static let hasVerticalAccuracy: UInt32 = 4
    static let hasSpeed: UInt32 = 8
    static let hasSpeedAccuracy: UInt32 = 16
    static let hasBearing: UInt32 = 32
    static let hasBearingAccuracy: UInt32 = 64
    /// The host's provider hint: fused, CoreLocation's one source.
    public static let providerFused: UInt32 = 3

    public var latitude: Double
    public var longitude: Double
    public var altitude: Double = 0
    public var horizontalAccuracy: Float
    public var verticalAccuracy: Float = 0
    public var speed: Float = 0
    public var speedAccuracy: Float = 0
    public var bearing: Float = 0
    public var bearingAccuracy: Float = 0
    public var timeMs: Int64
    public var elapsedRealtimeNs: Int64
    public var valid: UInt32

    /// Nil for a reading without a horizontal accuracy, which CoreLocation gives no position for. `now` is the wall
    /// clock and `boottime` the guest's CLOCK_BOOTTIME, both read at the same moment.
    public init?(_ reading: LocationReading, now: Date, boottime: Int64) {
        guard reading.horizontalAccuracy >= 0, reading.latitude.isFinite, reading.longitude.isFinite else { return nil }
        latitude = reading.latitude
        longitude = reading.longitude
        horizontalAccuracy = Float(reading.horizontalAccuracy)
        valid = Self.hasHorizontalAccuracy
        if reading.verticalAccuracy >= 0 {
            altitude = reading.altitude
            verticalAccuracy = Float(reading.verticalAccuracy)
            valid |= Self.hasAltitude | Self.hasVerticalAccuracy
        }
        if reading.speed >= 0 {
            speed = Float(reading.speed)
            valid |= Self.hasSpeed
        }
        if reading.speedAccuracy >= 0 {
            speedAccuracy = Float(reading.speedAccuracy)
            valid |= Self.hasSpeedAccuracy
        }
        if reading.course >= 0 {
            bearing = Float(reading.course)
            valid |= Self.hasBearing
        }
        if reading.courseAccuracy >= 0 {
            bearingAccuracy = Float(reading.courseAccuracy)
            valid |= Self.hasBearingAccuracy
        }
        timeMs = Int64((reading.timestamp.timeIntervalSince1970 * 1000).rounded(.down))
        let age = max(0, now.timeIntervalSince(reading.timestamp))
        elapsedRealtimeNs = min(boottime, max(0, boottime - Int64((age * 1e9).rounded())))
    }
}
