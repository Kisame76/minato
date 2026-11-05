import Foundation
import Testing

@testable import APKSession

/// ADR-0063 decision 3: a CLLocation as `struct apkrt_location_fix`: CoreLocation's negative sentinels clear the
/// field's APKRT_LOCATION_HAS_* bit, and elapsedRealtimeNanos is the fix's age taken from the guest's CLOCK_BOOTTIME.
@Suite struct LocationFixTests {
    private let now = Date(timeIntervalSince1970: 1_800_000_000)

    private func reading(_ change: (inout LocationReading) -> Void = { _ in }) -> LocationReading {
        var reading = LocationReading(latitude: 48.137, longitude: 11.575, altitude: 519.5, horizontalAccuracy: 5,
                                      verticalAccuracy: 3, speed: 1.5, speedAccuracy: 0.5, course: 90,
                                      courseAccuracy: 10, timestamp: now.addingTimeInterval(-2))
        change(&reading)
        return reading
    }

    @Test func aFullFixKeepsEveryFieldAndSetsEveryBit() throws {
        let fix = try #require(LocationFixValues(reading(), now: now, boottime: 10_000_000_000))
        #expect(fix.latitude == 48.137 && fix.longitude == 11.575 && fix.altitude == 519.5)
        #expect(fix.horizontalAccuracy == 5 && fix.verticalAccuracy == 3)
        #expect(fix.speed == 1.5 && fix.speedAccuracy == 0.5 && fix.bearing == 90 && fix.bearingAccuracy == 10)
        #expect(fix.timeMs == 1_799_999_998_000)
        #expect(fix.elapsedRealtimeNs == 8_000_000_000)
        #expect(fix.valid == 127)
    }

    @Test func aNegativeHorizontalAccuracyIsNoFix() {
        #expect(LocationFixValues(reading { $0.horizontalAccuracy = -1 }, now: now, boottime: 10) == nil)
    }

    @Test func eachNegativeSentinelClearsItsBitAndItsValue() throws {
        let fix = try #require(LocationFixValues(reading {
            $0.verticalAccuracy = -1
            $0.speed = -1
            $0.speedAccuracy = -1
            $0.course = -1
            $0.courseAccuracy = -1
        }, now: now, boottime: 10_000_000_000))
        // Only HAS_HORIZONTAL_ACCURACY: an altitude without a vertical accuracy is CoreLocation's invalid one.
        #expect(fix.valid == 2)
        #expect(fix.altitude == 0 && fix.verticalAccuracy == 0 && fix.speed == 0 && fix.bearing == 0)
    }

    @Test func aFixFromTheFutureOrOlderThanTheBootIsClamped() throws {
        let future = try #require(LocationFixValues(reading { $0.timestamp = now.addingTimeInterval(1) }, now: now,
                                                    boottime: 5))
        #expect(future.elapsedRealtimeNs == 5)
        let old = try #require(LocationFixValues(reading(), now: now, boottime: 1_000))
        #expect(old.elapsedRealtimeNs == 0)
    }
}
